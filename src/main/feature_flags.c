#include "feature_flags.h"

#include <stdint.h>

#include "esp_log.h"
#include "nvs.h"

#define FEATURES_NAMESPACE "radar" /* existing settings namespace */

static const char *TAG = "Features";

static const char *const kKeys[FEATURE_COUNT] = {
    "feat_seen", "feat_current", "feat_reg", "feat_ops", "ui_dark"};
static const char *const kNames[FEATURE_COUNT] = {
    "Seen Logging", "Current Aircraft", "Registered Aircraft", "Registered Operators", "Dark Mode"};

/* Defaults apply until Features_Init() runs and whenever a key is absent. */
static volatile bool s_on[FEATURE_COUNT] = {true, true, true, true, false};

void Features_Init(void)
{
    nvs_handle_t h;
    if (nvs_open(FEATURES_NAMESPACE, NVS_READONLY, &h) != ESP_OK)
        return; /* namespace not created yet: keep defaults */
    for (int i = 0; i < FEATURE_COUNT; i++) {
        uint8_t v;
        if (nvs_get_u8(h, kKeys[i], &v) == ESP_OK)
            s_on[i] = (v != 0);
    }
    nvs_close(h);
    ESP_LOGI(TAG, "seen=%d current=%d registered=%d operators=%d dark=%d",
             s_on[FEATURE_SEEN], s_on[FEATURE_CURRENT], s_on[FEATURE_REGISTERED],
             s_on[FEATURE_OPERATORS], s_on[FEATURE_DARK_MODE]);
}

bool Features_Get(FeatureId id)
{
    return ((unsigned)id < FEATURE_COUNT) ? s_on[id] : false;
}

const char *Features_Name(FeatureId id)
{
    return ((unsigned)id < FEATURE_COUNT) ? kNames[id] : "?";
}

bool Features_Set(FeatureId id, bool on)
{
    if ((unsigned)id >= FEATURE_COUNT)
        return false;
    if (s_on[id] == on)
        return true; /* no flash write for an unchanged value */
    nvs_handle_t h;
    if (nvs_open(FEATURES_NAMESPACE, NVS_READWRITE, &h) != ESP_OK)
        return false;
    esp_err_t err = nvs_set_u8(h, kKeys[id], on ? 1 : 0);
    if (err == ESP_OK)
        err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save %s failed: %s", kKeys[id], esp_err_to_name(err));
        return false;
    }
    s_on[id] = on;
    ESP_LOGW(TAG, "%s = %s", kNames[id], on ? "ON" : "OFF");
    return true;
}
