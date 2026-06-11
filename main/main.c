#include <stdio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <driver/gpio.h>

#include "storage.h"
#include "oled_display.h"
#include "wifi_manager.h"
#include "audio_io.h"

static const char *TAG = "MAIN";

#define BOOT_BUTTON_GPIO GPIO_NUM_0

void app_main(void)
{
    ESP_LOGI(TAG, "Initializing Phase 3 (Audio Loopback)...");

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

    // 3. Initialize Audio
    if (audio_io_init() != ESP_OK) {
        ESP_LOGE(TAG, "Audio hardware initialization failed.");
        oled_set_custom_message("AUDIO ERROR", "I2S mic/speaker init failed");
        oled_set_state(OLED_STATE_ERROR);
    }

    // 4. Configure Button (GPIO 0)
    gpio_config_t btn_cfg = {
        .pin_bit_mask = (1ULL << BOOT_BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn_cfg);

    // 5. Connect WiFi or start captive configuration portal
    esp_err_t wifi_err = wifi_manager_init(true);
    if (wifi_err != ESP_OK || !wifi_manager_is_connected()) {
        ESP_LOGI(TAG, "WiFi Onboarding AP Portal active. Configure via http://192.168.4.1");
        while (1) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    // Update Display state to Ready
    oled_set_state(OLED_STATE_READY);
    ESP_LOGI(TAG, "System Ready. Hold BOOT Button (GPIO0) for real-time mic loopback test.");

    int16_t audio_buffer[512];

    while (1) {
        // Poll BOOT button state (0 means pressed)
        if (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
            ESP_LOGI(TAG, "Loopback test started - Speak into the mic...");
            
            oled_set_state(OLED_STATE_LISTENING);
            audio_record_start();
            audio_play_start();

            while (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
                size_t samples_read = 0;
                esp_err_t err = audio_record_read(audio_buffer, 512, &samples_read, 100);
                if (err == ESP_OK && samples_read > 0) {
                    // Calculate RMS and feed to visualizer
                    float rms = audio_calculate_rms(audio_buffer, samples_read);
                    oled_set_mic_level(rms);

                    // Loopback directly to playback
                    size_t samples_written = 0;
                    audio_play_write(audio_buffer, samples_read, &samples_written, 100);
                }
                vTaskDelay(pdMS_TO_TICKS(5));
            }

            audio_record_stop();
            audio_play_stop();
            oled_set_state(OLED_STATE_READY);
            ESP_LOGI(TAG, "Loopback test stopped.");
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
