/*
 * Sentinel launcher integration — see include/managers/sentinel_integration.h
 * and docs/SENTINEL.md.
 */
#include "managers/sentinel_integration.h"

#include "sdkconfig.h"
#include "esp_partition.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_log.h"

static const char *TAG = "sentinel";

bool sentinel_launcher_boot_detected(void) {
    const esp_partition_t *run = esp_ota_get_running_partition();
    if (!run || run->type != ESP_PARTITION_TYPE_APP) {
        return false;
    }
    /* Running from an OTA slot (0x10..0x1F) => chain-loaded by a launcher.
     * Running from `factory` (0x00) => a direct flash of GhostESP's own image. */
    return run->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_MIN &&
           run->subtype <= ESP_PARTITION_SUBTYPE_APP_OTA_MAX;
}

bool sentinel_boot_detected(void) {
    if (!sentinel_launcher_boot_detected()) {
        return false;
    }
    const esp_partition_t *factory = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
    return factory == NULL;
}

void sentinel_return_to_launcher(void) {
    const esp_partition_t *otadata = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, NULL);
    if (otadata) {
        esp_err_t err = esp_partition_erase_range(otadata, 0, otadata->size);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "otadata erase failed: %s", esp_err_to_name(err));
        }
    } else {
        ESP_LOGW(TAG, "no otadata partition; restarting anyway");
    }
    esp_restart();
}

/*
 * Tell the Sentinel launcher where this firmware lives so it can offer in-launcher
 * updates from GitHub releases. Tab5 only (the asset names are Tab5 release assets);
 * harmless 256-byte rodata, kept in the image by the `-u sentinel_app_info` link flag
 * added in main/CMakeLists.txt under the same CONFIG_M5STACK_TAB5 guard.
 */
#if defined(CONFIG_M5STACK_TAB5)
#include "sentinel_app.h"
#if defined(CONFIG_M5STACK_TAB5_LANDSCAPE)
SENTINEL_APP_INFO("https://github.com/joshuawowk/GhostESP", "GhostESP-Tab5-Landscape.bin");
#else
SENTINEL_APP_INFO("https://github.com/joshuawowk/GhostESP", "GhostESP-Tab5-Portrait.bin");
#endif
#endif
