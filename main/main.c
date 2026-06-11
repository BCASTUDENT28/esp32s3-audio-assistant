#include <stdio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>

#include "storage.h"
#include "oled_display.h"
#include "wifi_manager.h"
#include "backend_client.h"
#include "telegram.h"

static const char *TAG = "MAIN";

void app_main(void)
{
    ESP_LOGI(TAG, "Initializing JARVIS-S3 Phase 4 (Telegram Integration)...");

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

    // 3. Connect WiFi or start captive configuration portal
    esp_err_t wifi_err = wifi_manager_init(true);
    if (wifi_err != ESP_OK || !wifi_manager_is_connected()) {
        ESP_LOGI(TAG, "WiFi Onboarding AP Portal active. Configure via http://192.168.4.1");
        while (1) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    // 4. Update Display state to Ready when connected
    oled_set_state(OLED_STATE_READY);
    ESP_LOGI(TAG, "WiFi Connected successfully!");

    // 5. Start Telegram bot listener task
    esp_err_t tg_err = telegram_init();
    if (tg_err == ESP_OK) {
        ESP_LOGI(TAG, "Telegram Bot client started successfully.");
    } else {
        ESP_LOGE(TAG, "Failed to start Telegram Bot client.");
    }

    // Standby loop
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
