#include <stdio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>

#include "storage.h"
#include "oled_display.h"
#include "wifi_manager.h"
#include "backend_client.h"

static const char *TAG = "MAIN";

void app_main(void)
{
    ESP_LOGI(TAG, "Initializing JARVIS-S3 Phase 3 (DeepSeek API Integration)...");

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

    // 5. Test DeepSeek HTTPS client
    char reply_buf[256] = {0};
    oled_set_custom_message("LLM QUERY", "Asking DeepSeek...");
    oled_set_state(OLED_STATE_THINKING);

    esp_err_t ds_err = backend_deepseek_chat(NULL, "Hello, answer in 5 words.", reply_buf, sizeof(reply_buf));
    if (ds_err == ESP_OK) {
        ESP_LOGI(TAG, "DeepSeek Reply: %s", reply_buf);
        oled_set_custom_message("DEEPSEEK SUCCESS", reply_buf);
    } else {
        ESP_LOGE(TAG, "DeepSeek query failed.");
        oled_set_custom_message("DEEPSEEK FAIL", "Query returned error");
        oled_set_state(OLED_STATE_ERROR);
    }

    // Hang here in standby loop after showing result
    vTaskDelay(pdMS_TO_TICKS(10000));
    oled_set_state(OLED_STATE_READY);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
