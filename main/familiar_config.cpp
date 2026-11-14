#include "familiar_config.h"

#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

#include <cstring>

static const char *TAG = "fam_cfg";
static const char *kNamespace = "familiar";

void familiar_cfg_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "nvs needs erase (%s); erasing", esp_err_to_name(err));
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) ESP_LOGE(TAG, "nvs_flash_init: %s", esp_err_to_name(err));
}

bool familiar_cfg_get(const char *key, char *out, size_t out_sz, const char *def)
{
    if (!out || out_sz == 0) return false;
    out[0] = '\0';

    nvs_handle_t h;
    if (nvs_open(kNamespace, NVS_READONLY, &h) != ESP_OK) {
        if (def) { strlcpy(out, def, out_sz); return out[0] != '\0'; }
        return false;
    }
    size_t len = out_sz;
    esp_err_t err = nvs_get_str(h, key, out, &len);
    nvs_close(h);

    if (err != ESP_OK || out[0] == '\0') {
        out[0] = '\0';
        if (def) { strlcpy(out, def, out_sz); return out[0] != '\0'; }
        return false;
    }
    return true;
}

void familiar_cfg_set(const char *key, const char *value)
{
    nvs_handle_t h;
    if (nvs_open(kNamespace, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open rw failed for %s", key);
        return;
    }
    esp_err_t err = nvs_set_str(h, key, value ? value : "");
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) ESP_LOGE(TAG, "set %s: %s", key, esp_err_to_name(err));
}

bool familiar_cfg_is_configured(void)
{
    char ssid[FAMILIAR_SSID_MAX + 1];
    char key[FAMILIAR_KEY_MAX + 1];
    bool have_ssid = familiar_cfg_get(FAMILIAR_K_SSID, ssid, sizeof(ssid), nullptr);
    bool have_key = familiar_cfg_get(FAMILIAR_K_APIKEY, key, sizeof(key), nullptr);
    return have_ssid && have_key;
}

void familiar_cfg_reset(void)
{
    nvs_handle_t h;
    if (nvs_open(kNamespace, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_all(h);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGW(TAG, "familiar settings wiped");
}
