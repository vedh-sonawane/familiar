#pragma once

// FAMILIAR networking. The ESP32-C3 has a single 2.4 GHz radio shared between
// Wi-Fi and BLE, and this firmware runs NimBLE tuned to the bone. Per the spec,
// Wi-Fi only comes up while a Familiar screen is open (BLE down at that point),
// mirroring how NFC/radio are enable-on-demand elsewhere.

#ifdef __cplusplus
extern "C" {
#endif

// Bring the Wi-Fi driver up in STA mode and tear it back down, logging free heap
// and largest free block at each step. No AP association is attempted:
// esp_wifi_init()/esp_wifi_start() allocate the bulk of the driver RAM regardless
// of connection, so this is the number that decides whether a TLS handshake can
// still fit afterward. Run it from the `wifiprobe` console command.
void familiar_net_probe(void);

// esp_console command wrapper (registered in hal_console.cpp).
int familiar_wifiprobe_cmd(int argc, char **argv);

// Connect to the Wi-Fi stored in NVS (STA). Releases the BLE controller's RAM
// for Wi-Fi first (BLE returns on the next reboot). Returns true once an IP is
// obtained, false on timeout/failure. `out_err` (optional, >=48 bytes) gets a
// short human-readable reason on failure.
bool familiar_net_sta_connect(int timeout_ms, char *out_err, int out_err_sz);
void familiar_net_sta_disconnect(void);
bool familiar_net_sta_has_ip(void);

// Streaming callbacks for a Claude call. on_delta receives decoded reply text as
// it arrives (never the raw SSE); on_status receives short lifecycle strings
// ("connecting", "thinking", an error message, etc.) for the UI. Both run on the
// network task - they must not touch LVGL directly; copy into shared state.
typedef void (*familiar_delta_cb)(const char *text, int len, void *ctx);
typedef void (*familiar_status_cb)(const char *status, void *ctx);

// POST one user prompt to api.anthropic.com/v1/messages (stream=true) using the
// stored API key + chat model, streaming decoded text to on_delta. Returns 0 on
// success, negative on error (a friendly reason is sent via on_status first).
// Assumes Wi-Fi is already connected. Logs free heap at each stage.
int familiar_net_claude_stream(const char *prompt,
                               familiar_delta_cb on_delta,
                               familiar_status_cb on_status, void *ctx);

#ifdef __cplusplus
}
#endif
