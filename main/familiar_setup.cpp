#include "familiar_setup.h"
#include "familiar_config.h"

#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_http_server.h"
#include "esp_bt.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include <cstring>
#include <cstdio>

static const char *TAG = "fam_setup";

static httpd_handle_t s_server = nullptr;
static esp_netif_t   *s_ap_netif = nullptr;
static char           s_ssid[20] = {0};
static char           s_status[64] = "idle";
static esp_event_handler_instance_t s_evt_inst = nullptr;
static volatile bool  s_saved = false;
static volatile int   s_clients = 0;
static bool           s_running = false;

// ---- the page (dark, Familiar-themed; plain ASCII, no external assets) --------

static const char kPageHead[] =
    "<!doctype html><html><head><meta name=viewport "
    "content='width=device-width,initial-scale=1'><title>Familiar setup</title>"
    "<style>"
    "*{box-sizing:border-box}"
    "body{margin:0;background:#050505;color:#f4f4ef;font-family:-apple-system,"
    "Segoe UI,Roboto,sans-serif;padding:22px;max-width:460px;margin:auto}"
    "h1{font-size:22px;margin:4px 0 2px}.sub{color:#b087b1;margin:0 0 18px;font-size:14px}"
    "label{display:block;margin:14px 0 5px;color:#bcbcb5;font-size:13px}"
    "input{width:100%;padding:12px;border-radius:10px;border:1px solid #422a43;"
    "background:#120a13;color:#f4f4ef;font-size:16px}"
    "button{margin-top:22px;width:100%;padding:14px;border:0;border-radius:12px;"
    "background:#664167;color:#fff;font-size:17px;font-weight:600}"
    "button:active{background:#b087b1}.hint{color:#696964;font-size:12px;margin-top:8px}"
    ".card{border:1px solid #422a43;border-radius:16px;padding:18px;background:#0b0c0b}"
    "</style></head><body>";

static const char kForm[] =
    "<h1>Familiar</h1><p class=sub>the badge that thinks, builds &amp; remembers</p>"
    "<form class=card method=POST action=/save>"
    "<label>Wi-Fi network (2.4GHz)</label><input name=ssid maxlength=32 required "
    "placeholder='Home Wi-Fi'>"
    "<label>Wi-Fi password</label><input name=pass type=password maxlength=64 "
    "placeholder='leave blank if open'>"
    "<label>Anthropic API key</label><input name=apikey maxlength=200 required "
    "placeholder='sk-ant-...'>"
    "<button type=submit>Wake Familiar</button>"
    "<p class=hint>Stored only on your badge. 2.4GHz Wi-Fi only (the badge has no "
    "5GHz radio).</p></form></body></html>";

static const char kSaved[] =
    "<div class=card><h1>Saved</h1><p class=sub>Familiar is waking up.</p>"
    "<p>You can close this page and disconnect from the badge's Wi-Fi. Follow the "
    "badge screen from here.</p></div></body></html>";

// ---- tiny form parsing --------------------------------------------------------

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// URL-decode src into dst (NUL-terminated, bounded).
static void url_decode(const char *src, char *dst, size_t dst_sz)
{
    size_t o = 0;
    for (size_t i = 0; src[i] && o + 1 < dst_sz; i++) {
        if (src[i] == '+') {
            dst[o++] = ' ';
        } else if (src[i] == '%' && hexval(src[i + 1]) >= 0 && hexval(src[i + 2]) >= 0) {
            dst[o++] = (char)(hexval(src[i + 1]) * 16 + hexval(src[i + 2]));
            i += 2;
        } else {
            dst[o++] = src[i];
        }
    }
    dst[o] = '\0';
}

// Extract field `name` from an application/x-www-form-urlencoded body.
static bool form_field(const char *body, const char *name, char *out, size_t out_sz)
{
    char key[40];
    int kn = snprintf(key, sizeof(key), "%s=", name);
    const char *p = body;
    while (p && *p) {
        if (strncmp(p, key, kn) == 0 && (p == body || p[-1] == '&')) {
            const char *val = p + kn;
            const char *amp = strchr(val, '&');
            size_t vlen = amp ? (size_t)(amp - val) : strlen(val);
            char raw[256];
            if (vlen >= sizeof(raw)) vlen = sizeof(raw) - 1;
            memcpy(raw, val, vlen);
            raw[vlen] = '\0';
            url_decode(raw, out, out_sz);
            return true;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
    out[0] = '\0';
    return false;
}

// ---- handlers -----------------------------------------------------------------

static esp_err_t get_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send_chunk(req, kPageHead, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, kForm, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, nullptr, 0);
    return ESP_OK;
}

static esp_err_t post_save(httpd_req_t *req)
{
    int total = req->content_len;
    if (total <= 0 || total > 1200) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad form");
        return ESP_FAIL;
    }
    // Static, not on the stack: the httpd task stack is small and this plus the
    // NVS writes below would overflow it. Only one POST is handled at a time.
    static char body[1216];
    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, body + got, total - got);
        if (r <= 0) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv"); return ESP_FAIL; }
        got += r;
    }
    body[got] = '\0';

    char ssid[FAMILIAR_SSID_MAX + 1] = {0};
    char pass[FAMILIAR_PASS_MAX + 1] = {0};
    char key[FAMILIAR_KEY_MAX + 1] = {0};
    form_field(body, "ssid", ssid, sizeof(ssid));
    form_field(body, "pass", pass, sizeof(pass));
    form_field(body, "apikey", key, sizeof(key));

    if (ssid[0] == '\0' || key[0] == '\0') {
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req,
            "<body style='background:#050505;color:#f4f4ef;font-family:sans-serif;padding:22px'>"
            "<h2>Missing Wi-Fi name or API key.</h2><a style='color:#b087b1' href=/>Go back</a>",
            HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    familiar_cfg_set(FAMILIAR_K_SSID, ssid);
    familiar_cfg_set(FAMILIAR_K_PASS, pass);
    familiar_cfg_set(FAMILIAR_K_APIKEY, key);
    ESP_LOGI(TAG, "saved ssid='%s' (pass %d chars, key %d chars)",
             ssid, (int)strlen(pass), (int)strlen(key));
    s_saved = true;

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send_chunk(req, kPageHead, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, kSaved, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, nullptr, 0);
    return ESP_OK;
}

// Catch-all: send captive-portal probes (and any stray path) to the form.
static esp_err_t get_redirect(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, nullptr, 0);
    return ESP_OK;
}

static void wifi_evt(void *, esp_event_base_t base, int32_t id, void *)
{
    if (base != WIFI_EVENT) return;
    if (id == WIFI_EVENT_AP_STACONNECTED) s_clients++;
    else if (id == WIFI_EVENT_AP_STADISCONNECTED && s_clients > 0) s_clients--;
}

// ---- lifecycle (stepped so the UI renders between steps) ----------------------

static int s_step = -1;

void familiar_setup_begin(void)
{
    if (s_running) return;
    s_saved = false;
    s_clients = 0;
    s_step = 0;
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_ssid, sizeof(s_ssid), "Familiar-%02X%02X", mac[4], mac[5]);
    snprintf(s_status, sizeof(s_status), "next: bt-release");
}

// Each case ends by naming the NEXT step in s_status, which the UI renders before
// that step runs. So if a step crashes, the badge is frozen showing exactly which
// step was about to run, plus the live heap - no serial needed.
int familiar_setup_step(void)
{
    switch (s_step) {
    case 0: {   // reclaim BLE controller RAM for Wi-Fi (Familiar never uses BLE)
        esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
        s_step = 1; snprintf(s_status, sizeof(s_status), "next: netif");
        return 0;
    }
    case 1: {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_init());
        esp_err_t le = esp_event_loop_create_default();
        if (le != ESP_OK && le != ESP_ERR_INVALID_STATE) ESP_LOGW(TAG, "evt loop: %s", esp_err_to_name(le));
        if (!s_ap_netif) s_ap_netif = esp_netif_create_default_wifi_ap();
        s_step = 2; snprintf(s_status, sizeof(s_status), "next: wifi-init");
        return 0;
    }
    case 2: {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        esp_err_t err = esp_wifi_init(&cfg);
        if (err != ESP_OK) { snprintf(s_status, sizeof(s_status), "init fail: %s", esp_err_to_name(err)); s_step = -1; return -1; }
        esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_evt, nullptr, &s_evt_inst);
        s_step = 3; snprintf(s_status, sizeof(s_status), "next: wifi-start");
        return 0;
    }
    case 3: {
        wifi_config_t ap = {};
        size_t n = strlen(s_ssid);
        memcpy(ap.ap.ssid, s_ssid, n);
        ap.ap.ssid_len = n;
        ap.ap.channel = 1;
        ap.ap.max_connection = 2;
        ap.ap.authmode = WIFI_AUTH_OPEN;   // open setup AP; no secrets travel over it
        esp_wifi_set_storage(WIFI_STORAGE_RAM);
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_mode(WIFI_MODE_AP));
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_config(WIFI_IF_AP, &ap));
        esp_err_t err = esp_wifi_start();
        if (err != ESP_OK) { snprintf(s_status, sizeof(s_status), "start fail: %s", esp_err_to_name(err)); s_step = -1; return -1; }
        esp_wifi_set_max_tx_power(40);   // smaller TX spike (BLE-tuned supply)
        s_step = 4; snprintf(s_status, sizeof(s_status), "next: httpd");
        return 0;
    }
    case 4: {
        httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
        hc.lru_purge_enable = true;
        hc.uri_match_fn = httpd_uri_match_wildcard;
        hc.max_uri_handlers = 6;
        hc.max_open_sockets = 2;      // one phone; keep LWIP socket demand low
        hc.stack_size = 6144;         // generous: the POST handler + NVS writes need room
        if (httpd_start(&s_server, &hc) != ESP_OK) {
            snprintf(s_status, sizeof(s_status), "httpd fail");
            esp_wifi_stop();
            s_step = -1; return -1;
        }
        httpd_uri_t root = { "/", HTTP_GET, get_root, nullptr };
        httpd_uri_t save = { "/save", HTTP_POST, post_save, nullptr };
        httpd_uri_t any  = { "/*", HTTP_GET, get_redirect, nullptr };
        httpd_register_uri_handler(s_server, &root);
        httpd_register_uri_handler(s_server, &save);
        httpd_register_uri_handler(s_server, &any);
        s_running = true;
        s_step = 5;
        snprintf(s_status, sizeof(s_status), "AP OK");
        ESP_LOGI(TAG, "setup AP '%s' up at http://192.168.4.1/", s_ssid);
        return 1;
    }
    default:
        return s_running ? 1 : -1;
    }
}

const char *familiar_setup_status_str(void) { return s_status; }

void familiar_setup_stop(void)
{
    if (!s_running) return;
    if (s_server) { httpd_stop(s_server); s_server = nullptr; }
    if (s_evt_inst) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_evt_inst);
        s_evt_inst = nullptr;
    }
    esp_wifi_stop();
    esp_wifi_deinit();
    if (s_ap_netif) { esp_netif_destroy_default_wifi(s_ap_netif); s_ap_netif = nullptr; }
    s_running = false;
    s_clients = 0;
    ESP_LOGI(TAG, "setup AP down");
}

const char *familiar_setup_ap_ssid(void) { return s_ssid; }
const char *familiar_setup_ap_ip(void)   { return "192.168.4.1"; }
int         familiar_setup_client_count(void) { return s_clients; }
bool        familiar_setup_saved(void)   { return s_saved; }
