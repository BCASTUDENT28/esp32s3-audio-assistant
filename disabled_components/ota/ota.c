#include "ota.h"
#include "oled_display.h"

#include <string.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_http_client.h>
#include <esp_https_ota.h>
#include <esp_crt_bundle.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char *TAG = "OTA";

static void ota_task(void *pvParameter)
{
    char *url = (char *)pvParameter;
    ESP_LOGI(TAG, "Starting background OTA update process...");

    // Set OLED to OTA warning state
    oled_set_state(OLED_STATE_OTA);

    esp_http_client_config_t http_config = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = true,
        .timeout_ms = 20000,
    };

    esp_https_ota_config_t ota_config = {
        .http_config = &http_config,
    };

    esp_err_t ret = esp_https_ota(&ota_config);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "OTA firmware update successful. Rebooting in 3 seconds...");
        vTaskDelay(pdMS_TO_TICKS(3000));
        esp_restart();
    } else {
        ESP_LOGE(TAG, "OTA firmware update failed: %s", esp_err_to_name(ret));
        oled_set_custom_message("OTA FAILED", "Check update link or connection.");
        oled_set_state(OLED_STATE_ERROR);
    }

    free(url);
    vTaskDelete(NULL);
}

esp_err_t ota_start(const char *url)
{
    if (url == NULL || strlen(url) == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    char *url_copy = strdup(url);
    if (!url_copy) {
        return ESP_ERR_NO_MEM;
    }

    // Spawn OTA flash task
    BaseType_t ret = xTaskCreate(ota_task, "ota_update_task", 8192, url_copy, 5, NULL);
    if (ret != pdPASS) {
        free(url_copy);
        return ESP_FAIL;
    }

    return ESP_OK;
}
