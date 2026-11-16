#include "familiar_net.h"
#include "familiar_config.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "esp_bt.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_tls.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "esp_pm.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

#include <ctime>
#include <fcntl.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include <cstdio>
#include <cstring>
#include <cstdarg>

static const char *TAG = "fam_net";

void familiar_net_log_reset(void)
{
    FILE *f = fopen("/littlefs/famlog.txt", "w");
    if (f) fclose(f);
}

void familiar_net_log(const char *fmt, ...)
{
    FILE *f = fopen("/littlefs/famlog.txt", "a");
    if (!f) return;
    fprintf(f, "%u ", (unsigned)(xTaskGetTickCount() * portTICK_PERIOD_MS));
    va_list ap; va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

// Internal-RAM free size and largest contiguous block - the pair that matters on
// a no-PSRAM part, since a TLS handshake needs one big contiguous buffer, not
// just total free bytes.
static void log_heap(const char *stage)
{
    unsigned freeb = (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    unsigned largest = (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "heap %-16s free=%u largest=%u", stage, freeb, largest);
}

void familiar_net_probe(void)
{
    log_heap("before");

    // Wi-Fi requires NVS for calibration/state. The firmware doesn't init NVS
    // itself (hal_config is LittleFS-backed), so do it here; idempotent.
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    log_heap("after nvs");

    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_init());
    esp_err_t le = esp_event_loop_create_default();
    if (le != ESP_OK && le != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "event loop: %s", esp_err_to_name(le));
    }
    esp_netif_t *sta = esp_netif_create_default_wifi_sta();
    log_heap("after netif");

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    ESP_LOGI(TAG, "esp_wifi_init: %s", esp_err_to_name(err));
    log_heap("after wifi_init");

    if (err == ESP_OK) {
        esp_wifi_set_storage(WIFI_STORAGE_RAM);   // don't touch NVS for this probe
        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_err_t se = esp_wifi_start();
        ESP_LOGI(TAG, "esp_wifi_start: %s", esp_err_to_name(se));
        esp_wifi_set_max_tx_power(40);   // 10 dBm: shrink the brownout-inducing spike
        log_heap("WIFI UP (sta)");

        esp_wifi_stop();
        esp_wifi_deinit();
    }

    if (sta) esp_netif_destroy_default_wifi(sta);
    log_heap("after teardown");
    ESP_LOGI(TAG, "probe done - compare 'WIFI UP' largest vs TLS need (~40KB)");
}

int familiar_wifiprobe_cmd(int /*argc*/, char ** /*argv*/)
{
    familiar_net_probe();
    return 0;
}

// ======================= STA connect + Anthropic client ======================

static esp_pm_lock_handle_t s_pm_lock = nullptr;   // keeps APB at 80 MHz during a session
static EventGroupHandle_t s_wifi_eg = nullptr;
static esp_netif_t       *s_sta_netif = nullptr;
static esp_event_handler_instance_t s_sta_any = nullptr, s_sta_got_ip = nullptr;
static bool               s_sta_up = false;
static volatile bool      s_have_ip = false;
static int                s_retries = 0;

static const int kBitConnected = BIT0;
static const int kBitFailed    = BIT1;

static volatile int s_last_reason = 0;

// The connect loop in familiar_net_sta_connect drives every esp_wifi_connect()
// call itself, spaced out. The handler must NOT auto-reconnect - it only records
// the disconnect reason and signals "this attempt failed" so the loop can back
// off before trying again. Hammering connect() here (the old <10 rapid-retry
// loop) is exactly what confuses WPA3-SAE and trips the router's rate-limiter,
// producing the r2/r205/r201 churn we saw. One deliberate attempt at a time.
static void sta_evt(void *, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (data) s_last_reason = ((wifi_event_sta_disconnected_t *)data)->reason;
        s_have_ip = false;
        xEventGroupSetBits(s_wifi_eg, kBitFailed);
    }
}

static void ip_evt(void *, esp_event_base_t, int32_t, void *)
{
    s_retries = 0;
    s_have_ip = true;
    xEventGroupSetBits(s_wifi_eg, kBitConnected);
}

bool familiar_net_sta_connect(int timeout_ms, char *out_err, int out_err_sz)
{
    if (out_err && out_err_sz) out_err[0] = '\0';
    if (s_sta_up && s_have_ip) return true;

    char ssid[FAMILIAR_SSID_MAX + 1] = {0};
    char pass[FAMILIAR_PASS_MAX + 1] = {0};
    if (!familiar_cfg_get(FAMILIAR_K_SSID, ssid, sizeof(ssid), nullptr)) {
        if (out_err) strlcpy(out_err, "no Wi-Fi set", out_err_sz);
        return false;
    }
    familiar_cfg_get(FAMILIAR_K_PASS, pass, sizeof(pass), "");

    // Keep the APB bus clock at 80 MHz for the whole session. A plugged-in computer
    // holds this lock through the USB-Serial-JTAG peripheral; without it, once the
    // badge goes idle between chat turns the clock scales down and the Wi-Fi link
    // goes stale, so follow-up turns could not reconnect (worked tethered, hung on
    // battery/charger). APB (not CPU) is what the radio needs, and it draws less
    // current than a CPU lock. Held until reboot (Familiar reboots on Wi-Fi exit).
#if CONFIG_PM_ENABLE
    if (!s_pm_lock) esp_pm_lock_create(ESP_PM_APB_FREQ_MAX, 0, "familiar", &s_pm_lock);
    if (s_pm_lock) esp_pm_lock_acquire(s_pm_lock);
#endif

    // Reclaim BLE RAM for Wi-Fi (same trade as setup; reboot restores BLE).
    esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
    log_heap("sta before");

    if (!s_wifi_eg) s_wifi_eg = xEventGroupCreate();
    xEventGroupClearBits(s_wifi_eg, kBitConnected | kBitFailed);
    s_retries = 0;

    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_init());
    esp_err_t le = esp_event_loop_create_default();
    if (le != ESP_OK && le != ESP_ERR_INVALID_STATE) ESP_LOGW(TAG, "evt: %s", esp_err_to_name(le));
    if (!s_sta_netif) s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&cfg);
    if (err != ESP_OK) { if (out_err) strlcpy(out_err, "wifi init failed", out_err_sz); return false; }

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &sta_evt, nullptr, &s_sta_any);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &ip_evt, nullptr, &s_sta_got_ip);

    wifi_config_t wc = {};
    strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, pass, sizeof(wc.sta.password));
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;   // find the 2.4 GHz BSS wherever it is
    // WPA3-Personal (e.g. Bell routers): the C3 stalls without PMF + H2E SAE.
    wc.sta.pmf_cfg.capable = true;
    wc.sta.pmf_cfg.required = false;
    wc.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;       // accept hash-to-element and hunt-and-peck
    s_last_reason = 0;
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_config(WIFI_IF_STA, &wc));
    err = esp_wifi_start();
    if (err != ESP_OK) { if (out_err) strlcpy(out_err, "wifi start failed", out_err_sz); return false; }
    // TX power: 52 = 13 dBm. Association is reliable now via the BSSID pin, so the
    // high 18 dBm setting is not needed, and its bigger current spike browns out
    // weak AA batteries. 13 dBm still reaches the router fine at the observed RSSI.
    esp_wifi_set_max_tx_power(52);
    // Disable Wi-Fi power-save. With the default min-modem power save the radio
    // sleeps between beacons and drops the TCP SYN-ACK, so UDP (DNS, SNTP) works
    // but the TCP connect to Anthropic times out (le0x8006). We're on USB/wall
    // power during a Familiar session, so keep the radio awake for low latency.
    esp_wifi_set_ps(WIFI_PS_NONE);
    s_sta_up = true;

    // Diagnostic + reliability: scan for our SSID and log every matching BSS
    // (RSSI, channel, authmode) so the serial capture shows EXACTLY what the AP
    // is - weak signal vs WPA2/WPA3 vs band-steered mesh - then pin the connect
    // to the strongest BSS. Pinning skips the slow in-connect scan and locks onto
    // one AP, which makes WPA3-SAE association far steadier than hunting each try.
    // authmode: 3=WPA2_PSK 4=WPA/WPA2 6=WPA3_PSK 7=WPA2/WPA3 9=OWE.
    {
        wifi_scan_config_t scfg = {};
        scfg.ssid = (uint8_t *)ssid;
        scfg.show_hidden = true;
        esp_err_t se = esp_wifi_scan_start(&scfg, true);   // blocking (~2-4 s)
        uint16_t n = 0;
        esp_wifi_scan_get_ap_num(&n);
        ESP_LOGI(TAG, "scan '%s' -> %u BSS (%s)", ssid, n, esp_err_to_name(se));
        if (n > 10) n = 10;
        wifi_ap_record_t recs[10];
        if (n && esp_wifi_scan_get_ap_records(&n, recs) == ESP_OK) {
            int best = -1; int8_t best_rssi = -128;
            for (int i = 0; i < n; i++) {
                ESP_LOGI(TAG, "  BSS%d %02x:%02x:%02x:%02x:%02x:%02x ch%d rssi%d auth%d",
                    i, recs[i].bssid[0], recs[i].bssid[1], recs[i].bssid[2],
                    recs[i].bssid[3], recs[i].bssid[4], recs[i].bssid[5],
                    recs[i].primary, recs[i].rssi, (int)recs[i].authmode);
                if (recs[i].rssi > best_rssi) { best_rssi = recs[i].rssi; best = i; }
            }
            if (best >= 0) {
                memcpy(wc.sta.bssid, recs[best].bssid, 6);
                wc.sta.bssid_set = true;
                wc.sta.channel = recs[best].primary;
                esp_wifi_set_config(WIFI_IF_STA, &wc);
                ESP_LOGI(TAG, "pinned BSS%d ch%d rssi%d auth%d",
                         best, recs[best].primary, best_rssi, (int)recs[best].authmode);
            }
        }
    }

    // Patient, spaced connect loop (see sta_evt). WPA3-SAE on the C3 associates
    // far more reliably with one deliberate attempt at a time than with rapid
    // retries: kick off a connect, wait up to ~13 s for the real result, and on
    // failure pause ~2.5 s (letting the SAE state machine and the router settle)
    // before the next try. This runs on the chat_task, so blocking is fine.
    bool connected = false;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    int attempt = 0;
    while (xTaskGetTickCount() < deadline) {
        attempt++;
        xEventGroupClearBits(s_wifi_eg, kBitConnected | kBitFailed);
        esp_wifi_connect();
        EventBits_t bits = xEventGroupWaitBits(s_wifi_eg, kBitConnected | kBitFailed,
                                               pdTRUE, pdFALSE, pdMS_TO_TICKS(13000));
        if (bits & kBitConnected) { connected = true; break; }
        ESP_LOGW(TAG, "wifi attempt %d failed (reason %d); backing off 2.5s", attempt, s_last_reason);
        familiar_net_log("sta: assoc try %d fail r%d", attempt, s_last_reason);
        vTaskDelay(pdMS_TO_TICKS(2500));
    }
    log_heap("sta after");
    familiar_net_log("sta: %s", connected ? "associated" : "GAVE UP");
    if (connected) {
        // TLS cert validation checks notBefore/notAfter against the clock, but
        // the chip boots at 1970 - every cert looks "not yet valid" and the
        // handshake is rejected. Sync real time over NTP before any HTTPS call.
        time_t now = 0; time(&now);
        if (now < 1700000000) {   // still ~1970 -> need a clock over NTP
            // Several waits, not one: a single UDP query to the pool is easily
            // lost, which showed up on screen as "clock sync failed". Up to 4
            // waits of 8 s each (the SNTP client keeps retransmitting) makes it
            // robust without needing a bigger server list.
            esp_sntp_config_t sc = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
            if (esp_netif_sntp_init(&sc) == ESP_OK) {
                for (int i = 0; i < 4 && now < 1700000000; i++) {
                    esp_err_t se = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(8000));
                    time(&now);
                    ESP_LOGI(TAG, "sntp try %d: %s, time=%ld", i, esp_err_to_name(se), (long)now);
                }
                esp_netif_sntp_deinit();
            }
            if (now < 1700000000 && out_err)
                strlcpy(out_err, "clock sync failed", out_err_sz);
        }
        return now >= 1700000000;   // only usable once we have a real clock
    }
    if (out_err && out_err[0] == '\0') {
        // reason 201=AP not found (need the 2.4GHz SSID), 15=handshake/bad pw,
        // 2/204=auth, 205=conn fail. Surface it so failures are diagnosable.
        snprintf(out_err, out_err_sz, "Wi-Fi failed (r%d)", s_last_reason);
    }
    return false;
}

void familiar_net_sta_disconnect(void)
{
    if (!s_sta_up) return;
    if (s_sta_any)    { esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_sta_any); s_sta_any = nullptr; }
    if (s_sta_got_ip) { esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_sta_got_ip); s_sta_got_ip = nullptr; }
    esp_wifi_disconnect();
    esp_wifi_stop();
    esp_wifi_deinit();
    if (s_sta_netif) { esp_netif_destroy_default_wifi(s_sta_netif); s_sta_netif = nullptr; }
    s_sta_up = false;
    s_have_ip = false;
}

bool familiar_net_sta_has_ip(void) { return s_have_ip; }

// ---- minimal JSON string-value decoder for the SSE delta text ----------------

// Given a pointer just past `"text":"`, decode the JSON string into out (bounded),
// stopping at the closing quote. Handles the escapes Claude actually emits.
static int decode_json_str(const char *src, char *out, int out_sz)
{
    int o = 0;
    for (int i = 0; src[i] && o < out_sz - 1; i++) {
        char c = src[i];
        if (c == '"') break;                    // end of string
        if (c == '\\') {
            char n = src[++i];
            switch (n) {
                case 'n': out[o++] = '\n'; break;
                case 't': out[o++] = ' ';  break;
                case 'r': break;
                case 'u': {                      // \uXXXX -> keep ASCII, else '?'
                    int v = 0; for (int k = 0; k < 4 && src[i + 1]; k++) {
                        char h = src[++i]; v <<= 4;
                        v |= (h >= '0' && h <= '9') ? h - '0'
                           : (h >= 'a' && h <= 'f') ? h - 'a' + 10
                           : (h >= 'A' && h <= 'F') ? h - 'A' + 10 : 0;
                    }
                    out[o++] = (v >= 32 && v < 127) ? (char)v : '?';
                    break;
                }
                default: out[o++] = n; break;    // \" \\ \/ etc.
            }
        } else {
            out[o++] = c;
        }
    }
    out[o] = '\0';
    return o;
}

// Raw non-blocking TCP connect test to a literal IP:port with a timeout.
// Returns 0 on success, or a nonzero code (SO_ERROR value, or -1/-3) on failure.
// Used to tell "the network blocks TCP" apart from "Anthropic-specific issue".
static int tcp_probe(const char *ip, uint16_t port, int timeout_ms)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    struct sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = inet_addr(ip);
    int r = connect(fd, (struct sockaddr *)&a, sizeof(a));
    if (r == 0) { close(fd); return 0; }
    if (errno != EINPROGRESS) { int e = errno; close(fd); return e ? e : -2; }
    fd_set wf; FD_ZERO(&wf); FD_SET(fd, &wf);
    struct timeval tv; tv.tv_sec = timeout_ms / 1000; tv.tv_usec = (timeout_ms % 1000) * 1000;
    r = select(fd + 1, nullptr, &wf, nullptr, &tv);
    if (r <= 0) { close(fd); return -3; }   // timed out waiting for the SYN-ACK
    int so_err = 0; socklen_t sl = sizeof(so_err);
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_err, &sl);
    close(fd);
    return so_err;   // 0 = connected
}

struct StreamCtx {
    familiar_delta_cb on_delta;
    void *ctx;
    char line[1024];
    int  line_len;
};

// Feed raw bytes; split on newlines; for each `data:` line pull out every
// text_delta's text and hand it to the callback.
static void feed_sse(StreamCtx *s, const char *buf, int n)
{
    for (int i = 0; i < n; i++) {
        char c = buf[i];
        if (c == '\n') {
            s->line[s->line_len] = '\0';
            const char *p = s->line;
            if (strncmp(p, "data:", 5) == 0) {
                const char *td = strstr(p, "\"text_delta\"");
                if (td) {
                    const char *t = strstr(td, "\"text\":\"");
                    if (t) {
                        char dec[768];
                        int dn = decode_json_str(t + 8, dec, sizeof(dec));
                        if (dn && s->on_delta) s->on_delta(dec, dn, s->ctx);
                    }
                }
            }
            s->line_len = 0;
        } else if (s->line_len < (int)sizeof(s->line) - 1) {
            s->line[s->line_len++] = c;
        } else {
            s->line_len = 0;   // overlong line (shouldn't happen) - drop it
        }
    }
}

int familiar_net_claude_stream(const char *prompt, familiar_delta_cb on_delta,
                               familiar_status_cb on_status, void *ctx)
{
    char key[FAMILIAR_KEY_MAX + 1] = {0};
    char model[FAMILIAR_MODEL_MAX + 1] = {0};
    char s_resolved_ip[16] = {0};   // DNS result for api.anthropic.com, shown on TLS failure
    if (!familiar_cfg_get(FAMILIAR_K_APIKEY, key, sizeof(key), nullptr)) {
        if (on_status) on_status("no API key", ctx);
        return -1;
    }
    familiar_cfg_get(FAMILIAR_K_MODEL_CHAT, model, sizeof(model), FAMILIAR_DEFAULT_MODEL_CHAT);

    // Build the request body. The prompt is a fixed internal string (no user
    // free-text yet), so it needs no JSON escaping for the hello milestone.
    static char body[768];
    int blen = snprintf(body, sizeof(body),
        "{\"model\":\"%s\",\"max_tokens\":200,\"stream\":true,"
        "\"messages\":[{\"role\":\"user\",\"content\":\"%s\"}]}", model, prompt);

    log_heap("tls before");

    // Step 1 - resolve DNS explicitly (IPv4 only) BEFORE any TLS, so a resolution
    // failure is reported as DNS, not a vague "timeout". esp_tls_conn_new_sync
    // resolves internally, but then we can't tell DNS failure from a dropped SYN.
    {
        struct addrinfo hints = {};
        hints.ai_family = AF_INET;         // A records only (badge has no IPv6 route)
        hints.ai_socktype = SOCK_STREAM;
        struct addrinfo *res = nullptr;
        int ga = getaddrinfo("api.anthropic.com", "443", &hints, &res);
        if (ga != 0 || !res) {
            char msg[64];
            snprintf(msg, sizeof(msg), "DNS fail (e%d)", ga);
            ESP_LOGE(TAG, "getaddrinfo api.anthropic.com -> %d", ga);
            if (on_status) on_status(msg, ctx);
            if (res) freeaddrinfo(res);
            return -5;
        }
        struct sockaddr_in *sa = (struct sockaddr_in *)res->ai_addr;
        inet_ntoa_r(sa->sin_addr, s_resolved_ip, sizeof(s_resolved_ip));
        ESP_LOGI(TAG, "api.anthropic.com -> %s", s_resolved_ip);
        freeaddrinfo(res);
    }

    // Step 1.5 - raw TCP reachability test to Cloudflare (1.1.1.1:443), a host
    // unrelated to Anthropic. If this ALSO times out, the network is dropping the
    // badge's outbound TCP (router security), not an Anthropic-specific problem.
    int cf = tcp_probe("1.1.1.1", 443, 6000);
    int an = s_resolved_ip[0] ? tcp_probe(s_resolved_ip, 443, 8000) : -9;
    ESP_LOGI(TAG, "tcp 1.1.1.1:443 -> %d   %s:443 -> %d", cf, s_resolved_ip, an);

    // Step 2 - diagnostic TLS probe with PATIENT RETRY. On a freshly-associated
    // link the first TCP SYN to Anthropic is often dropped (le0x8006 timeout);
    // a single retry after a short settle almost always gets through. Same
    // philosophy as the Wi-Fi connect loop: a calm retry beats one shot.
    {
        bool tls_ok = false;
        esp_err_t le = ESP_OK; int te = 0, tf = 0;
        for (int attempt = 1; attempt <= 3 && !tls_ok; attempt++) {
            esp_tls_cfg_t tcfg = {};
            tcfg.crt_bundle_attach = esp_crt_bundle_attach;
            tcfg.timeout_ms = 20000;   // generous: cert burst + small window can be slow
            tcfg.addr_family = ESP_TLS_AF_INET;   // IPv4 only (badge has no IPv6 route)
            esp_tls_t *probe = esp_tls_init();
            int pr = probe ? esp_tls_conn_new_sync("api.anthropic.com", 17, 443, &tcfg, probe) : -1;
            if (pr == 1) { tls_ok = true; esp_tls_conn_destroy(probe); break; }
            le = ESP_OK; te = 0; tf = 0;
            esp_tls_error_handle_t eh = nullptr;
            if (probe && esp_tls_get_error_handle(probe, &eh) == ESP_OK && eh)
                le = esp_tls_get_and_clear_last_error(eh, &te, &tf);
            ESP_LOGW(TAG, "tls probe attempt %d: ret=%d le=0x%x mbed=-0x%x flags=0x%x",
                     attempt, pr, (unsigned)le, (unsigned)(-te), (unsigned)tf);
            if (probe) esp_tls_conn_destroy(probe);
            if (attempt < 3) vTaskDelay(pdMS_TO_TICKS(1500));
        }
        if (!tls_ok) {
            char msg[112];
            snprintf(msg, sizeof(msg), "le0x%x mb0x%x f0x%x an%d",
                     (unsigned)le, (unsigned)(-te), (unsigned)tf, an);
            (void)cf;
            if (on_status) on_status(msg, ctx);
            log_heap("tls after");
            return -6;
        }
        ESP_LOGI(TAG, "tls probe OK");
    }

    esp_http_client_config_t cfg = {};
    cfg.url = "https://api.anthropic.com/v1/messages";
    cfg.method = HTTP_METHOD_POST;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;   // ESP-IDF root CA bundle
    cfg.timeout_ms = 30000;
    cfg.buffer_size = 2048;
    cfg.buffer_size_tx = 1024;
    esp_http_client_handle_t cl = esp_http_client_init(&cfg);
    if (!cl) { if (on_status) on_status("http init failed", ctx); return -2; }

    esp_http_client_set_header(cl, "x-api-key", key);
    esp_http_client_set_header(cl, "anthropic-version", "2023-06-01");
    esp_http_client_set_header(cl, "content-type", "application/json");

    int rc = 0;
    esp_err_t err = esp_http_client_open(cl, blen);
    if (err != ESP_OK) {
        if (on_status) on_status("couldn't reach Claude", ctx);
        rc = -3; goto done;
    }
    if (esp_http_client_write(cl, body, blen) < 0) { if (on_status) on_status("send failed", ctx); rc = -4; goto done; }

    {
        int64_t clen = esp_http_client_fetch_headers(cl);
        (void)clen;
        int status = esp_http_client_get_status_code(cl);
        log_heap("tls connected");
        if (status != 200) {
            // Read a bit of the error body and surface a short reason.
            char ebuf[256];
            int en = esp_http_client_read(cl, ebuf, sizeof(ebuf) - 1);
            if (en < 0) en = 0;
            ebuf[en] = '\0';
            const char *m = strstr(ebuf, "\"message\":\"");
            char reason[80];
            if (m) { decode_json_str(m + 11, reason, sizeof(reason)); }
            else   { snprintf(reason, sizeof(reason), "HTTP %d", status); }
            ESP_LOGE(TAG, "claude %d: %s", status, ebuf);
            if (on_status) on_status(reason, ctx);
            rc = -5; goto done;
        }

        if (on_status) on_status("", ctx);   // clear status; deltas start now
        StreamCtx sc = {}; sc.on_delta = on_delta; sc.ctx = ctx;
        char rbuf[1024];
        while (true) {
            int n = esp_http_client_read(cl, rbuf, sizeof(rbuf));
            if (n <= 0) break;   // 0 = done, <0 = closed/error
            feed_sse(&sc, rbuf, n);
        }
    }

done:
    esp_http_client_close(cl);
    esp_http_client_cleanup(cl);
    log_heap("tls after");
    return rc;
}

int familiar_net_claude_stream_msgs(const char *system_prompt,
                                    const char *messages_json,
                                    familiar_delta_cb on_delta,
                                    familiar_status_cb on_status, void *ctx)
{
    char key[FAMILIAR_KEY_MAX + 1] = {0};
    char model[FAMILIAR_MODEL_MAX + 1] = {0};
    if (!familiar_cfg_get(FAMILIAR_K_APIKEY, key, sizeof(key), nullptr)) {
        if (on_status) on_status("no API key", ctx);
        return -1;
    }
    familiar_cfg_get(FAMILIAR_K_MODEL_CHAT, model, sizeof(model), FAMILIAR_DEFAULT_MODEL_CHAT);

    // Body holds the system prompt + the whole (bounded) conversation, plus the
    // server-side web_search tool so Claude can look up current facts instead of
    // hallucinating. The caller caps history so this stays under the buffer.
    static char body[2560];
    int blen = snprintf(body, sizeof(body),
        "{\"model\":\"%s\",\"max_tokens\":350,\"stream\":true,"
        "\"tools\":[{\"type\":\"web_search_20250305\",\"name\":\"web_search\",\"max_uses\":3}],"
        "\"system\":\"%s\",\"messages\":%s}",
        model, system_prompt ? system_prompt : "", messages_json);
    if (blen <= 0 || blen >= (int)sizeof(body)) {
        if (on_status) on_status("message too long", ctx);
        return -7;
    }
    log_heap("msg tls before");

    // Send the request straight over esp-tls, by hand, instead of esp_http_client.
    // The http_client wrapper returns ESP_ERR_HTTP_CONNECT (0x7002) whenever the
    // badge runs off a computer, yet a raw esp-tls connection to the SAME host
    // succeeds there (proven on-screen by the earlier probe). So we open the TLS
    // connection ourselves and write a minimal HTTP/1.1 request over it. Retried
    // twice on a transient connect/read failure - always before any reply byte is
    // delivered, so a retry can never duplicate text. Kept short (2 x 12 s) so a
    // dead link fails fast and the caller can reconnect Wi-Fi and try again.
    familiar_net_log("stream: start blen=%d", blen);
    for (int attempt = 1; attempt <= 2; attempt++) {
        familiar_net_log("stream: tls connect try %d", attempt);
        esp_tls_cfg_t tcfg = {};
        tcfg.crt_bundle_attach = esp_crt_bundle_attach;
        tcfg.timeout_ms = 12000;
        tcfg.addr_family = ESP_TLS_AF_INET;   // IPv4 only (badge has no IPv6 route)
        esp_tls_t *tls = esp_tls_init();
        if (!tls) { familiar_net_log("stream: tls_init NULL"); if (on_status) on_status("tls init failed", ctx); return -2; }

        if (esp_tls_conn_new_sync("api.anthropic.com", 17, 443, &tcfg, tls) != 1) {
            int te = 0, tf = 0; esp_err_t le = ESP_OK; esp_tls_error_handle_t eh = nullptr;
            if (esp_tls_get_error_handle(tls, &eh) == ESP_OK && eh)
                le = esp_tls_get_and_clear_last_error(eh, &te, &tf);
            esp_tls_conn_destroy(tls);
            char msg[96];
            snprintf(msg, sizeof(msg), "TLS le0x%x mb0x%x f0x%x", (unsigned)le, (unsigned)(-te), (unsigned)tf);
            ESP_LOGW(TAG, "tls connect attempt %d: %s", attempt, msg);
            familiar_net_log("stream: tls FAIL %s", msg);
            if (attempt < 2) { vTaskDelay(pdMS_TO_TICKS(1200)); continue; }
            if (on_status) on_status(msg, ctx);
            log_heap("msg tls after");
            return -6;
        }
        familiar_net_log("stream: tls connected");
        log_heap("msg tls connected");

        // Write the request: headers then the JSON body, looping on partial writes.
        char hdr[400];
        int hl = snprintf(hdr, sizeof(hdr),
            "POST /v1/messages HTTP/1.1\r\n"
            "Host: api.anthropic.com\r\n"
            "x-api-key: %s\r\n"
            "anthropic-version: 2023-06-01\r\n"
            "content-type: application/json\r\n"
            "content-length: %d\r\n"
            "connection: close\r\n\r\n", key, blen);
        bool wrote = true;
        for (int pass = 0; pass < 2 && wrote; pass++) {
            const char *p = (pass == 0) ? hdr : body;
            int len = (pass == 0) ? hl : blen, off = 0;
            while (off < len) {
                int w = esp_tls_conn_write(tls, p + off, len - off);
                if (w == ESP_TLS_ERR_SSL_WANT_READ || w == ESP_TLS_ERR_SSL_WANT_WRITE) continue;
                if (w < 0) { wrote = false; break; }
                off += w;
            }
        }
        if (!wrote) {
            esp_tls_conn_destroy(tls);
            ESP_LOGW(TAG, "tls write attempt %d failed", attempt);
            if (attempt < 2) { vTaskDelay(pdMS_TO_TICKS(1200)); continue; }
            if (on_status) on_status("send failed", ctx);
            log_heap("msg tls after");
            return -4;
        }

        // Read the response. Skip HTTP headers (up to the blank line) with a rolling
        // \r\n\r\n match, grab the status code from the first line, then feed the
        // rest to the SSE parser. feed_sse only reacts to "data:" lines, so any
        // chunked-transfer size lines pass through harmlessly.
        StreamCtx sc = {}; sc.on_delta = on_delta; sc.ctx = ctx;
        char hacc[128]; int hlen = 0, hmatch = 0, status_code = 0;
        bool hdr_done = false;
        char rbuf[1024];
        while (true) {
            int n = esp_tls_conn_read(tls, rbuf, sizeof(rbuf));
            if (n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE) continue;
            if (n <= 0) break;   // 0 = server closed (stream end), <0 = error/timeout
            int bstart = 0;
            if (!hdr_done) {
                int i = 0;
                for (; i < n; i++) {
                    char c = rbuf[i];
                    if (hlen < (int)sizeof(hacc) - 1) hacc[hlen++] = c;
                    if (c == '\r')      hmatch = (hmatch == 2) ? 3 : 1;
                    else if (c == '\n') hmatch = (hmatch == 1) ? 2 : (hmatch == 3) ? 4 : 0;
                    else                hmatch = 0;
                    if (hmatch == 4) {
                        hdr_done = true;
                        hacc[hlen] = '\0';
                        const char *sp = strchr(hacc, ' ');
                        if (sp) status_code = atoi(sp + 1);
                        if (status_code == 200 && on_status) on_status("", ctx);
                        i++;
                        break;
                    }
                }
                bstart = i;
                if (!hdr_done) continue;
            }
            if (status_code == 200) feed_sse(&sc, rbuf + bstart, n - bstart);
        }
        esp_tls_conn_destroy(tls);
        familiar_net_log("stream: status=%d", status_code);
        log_heap("msg tls after");
        if (status_code == 200) { familiar_net_log("stream: done ok"); return 0; }
        if (status_code > 0) {
            char msg[48]; snprintf(msg, sizeof(msg), "Claude error HTTP %d", status_code);
            if (on_status) on_status(msg, ctx);
            return -5;
        }
        ESP_LOGW(TAG, "no response attempt %d", attempt);
        familiar_net_log("stream: no response");
        if (attempt < 2) { vTaskDelay(pdMS_TO_TICKS(1200)); continue; }
        if (on_status) on_status("couldn't reach Claude", ctx);
        return -3;
    }
    return -3;
}
