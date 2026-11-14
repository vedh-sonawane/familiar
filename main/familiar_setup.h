#pragma once

// FAMILIAR first-run setup: the badge becomes a Wi-Fi access point and serves a
// one-page form where the owner enters their Wi-Fi credentials and Anthropic API
// key from a phone. Saved straight to NVS via familiar_config; nothing is ever
// put in source. Brought up only while the Familiar setup screen is open and torn
// down on exit, mirroring the firmware's enable-on-demand radio/NFC pattern.

#ifdef __cplusplus
extern "C" {
#endif

// The AP + HTTP portal is brought up one step per call so the badge UI can render
// (and show which step is next) between steps - crash-localizing without a serial
// cable. Call familiar_setup_begin() once, then familiar_setup_step() each tick
// until it returns non-zero.
void        familiar_setup_begin(void);
int         familiar_setup_step(void);   // 0 = in progress, 1 = AP up, -1 = failed
void        familiar_setup_stop(void);

// "Familiar-XXXX" (XXXX from the Wi-Fi MAC); valid after start.
const char *familiar_setup_ap_ssid(void);
const char *familiar_setup_ap_ip(void);   // "192.168.4.1"

// Count of phones currently associated to the setup AP (for a live hint).
int         familiar_setup_client_count(void);

// Flips true once the owner submits the form with a usable Wi-Fi SSID + API key.
bool        familiar_setup_saved(void);

// Human-readable last status of the AP bring-up ("AP OK", "init fail: ...", etc.)
// for on-screen diagnostics.
const char *familiar_setup_status_str(void);

#ifdef __cplusplus
}
#endif
