#include <stdio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>

#include "storage.h"
#include "oled_display.h"

static const char *TAG = "MAIN";

void app_main(void)
{
    ESP_LOGI(TAG, "Initializing JARVIS-S3 Phase 1 (Storage + OLED)...");

    // 1. Initialize Display
    if (oled_init() == ESP_OK) {
        oled_set_state(OLED_STATE_BOOT);
    }
    vTaskDelay(pdMS_TO_TICKS(1000));

    // 2. Initialize Secure Storage & SPIFFS
    if (storage_init() != ESP_OK) {
        ESP_LOGE(TAG, "Storage initialization failed.");
        oled_set_custom_message("STORAGE CRITICAL", "NVS/SPIFFS init failed");
        oled_set_state(OLED_STATE_ERROR);
        return;
    }

    // 3. Update Display state to Ready to show everything initialized successfully
    oled_set_state(OLED_STATE_READY);
    ESP_LOGI(TAG, "JARVIS-S3 Phase 1 Initialization successful. Standby mode active.");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
