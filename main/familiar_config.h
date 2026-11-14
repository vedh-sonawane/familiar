#pragma once

// FAMILIAR persisted settings (Wi-Fi creds, Anthropic API key, model choices).
//
// These live in the dedicated NVS partition (0x9000), NOT in LittleFS and NOT in
// source control: the captive setup portal writes them once and they survive
// firmware reflashes and the LittleFS reformat that a repartition causes. The
// spec is explicit - an API key or Wi-Fi password must never appear in source or
// git. `familiar_cfg_reset()` backs the "reset setup" option.

#include <cstddef>

#ifdef __cplusplus
extern "C" {
#endif

// Defaults for the two models (overridable via NVS). Chat wants speed, Forge
// wants better code. The spec's placeholder IDs (claude-haiku-5-5 /
// claude-sonnet-5-5) do not exist; these are the verified current IDs - Haiku
// 4.5 is the fastest/cheapest and ideal for a latency-sensitive badge chat.
#define FAMILIAR_DEFAULT_MODEL_CHAT  "claude-haiku-4-5"
#define FAMILIAR_DEFAULT_MODEL_FORGE "claude-sonnet-4-6"

// Field length caps (bytes, excluding the NUL the getters append).
#define FAMILIAR_SSID_MAX   32
#define FAMILIAR_PASS_MAX   64
#define FAMILIAR_KEY_MAX    200
#define FAMILIAR_MODEL_MAX  48

void familiar_cfg_init(void);   // ensure NVS is ready; safe to call repeatedly

// Copy a value into out (NUL-terminated). Returns true if a non-empty value was
// stored. `def` (may be NULL) is used when the key is missing; for api_key/ssid
// there is no default.
bool familiar_cfg_get(const char *key, char *out, size_t out_sz, const char *def);
void familiar_cfg_set(const char *key, const char *value);

// Keys.
#define FAMILIAR_K_SSID        "wifi_ssid"
#define FAMILIAR_K_PASS        "wifi_pass"
#define FAMILIAR_K_APIKEY      "api_key"
#define FAMILIAR_K_MODEL_CHAT  "model_chat"
#define FAMILIAR_K_MODEL_FORGE "model_forge"

// Configured == we have both a Wi-Fi SSID and an API key to work with.
bool familiar_cfg_is_configured(void);

// Wipe every Familiar setting ("reset setup"). Does not touch identity/wallet.
void familiar_cfg_reset(void);

#ifdef __cplusplus
}
#endif
