#include <stdio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <driver/gpio.h>

#include "storage.h"
#include "oled_display.h"
#include "wifi_manager.h"
#include "backend_client.h"
#include "telegram.h"
#include "audio_io.h"

static const char *TAG = "MAIN";

#define BOOT_BUTTON_GPIO GPIO_NUM_0

// WAV Header struct according to standard spec
struct WavHeader {
    char riff_header[4];     // Contains "RIFF"
    int32_t wav_size;        // Size of the file in bytes minus 8
    char wave_header[4];     // Contains "WAVE"
    char fmt_header[4];      // Contains "fmt "
    int32_t fmt_chunk_size;  // 16
    int16_t audio_format;    // 1 (PCM)
    int16_t num_channels;    // 1 (Mono)
    int32_t sample_rate;     // 16000
    int32_t byte_rate;       // 16000 * 1 * (16 / 8) = 32000
    int16_t block_align;     // 1 * (16 / 8) = 2
    int16_t bits_per_sample; // 16
    char data_header[4];     // Contains "data"
    int32_t data_bytes;      // Number of bytes in data chunk
};

// Update WAV header on SPIFFS file
static void write_wav_header(FILE *f, int32_t data_size)
{
    struct WavHeader header = {
        .riff_header = {'R', 'I', 'F', 'F'},
        .wav_size = data_size + 36,
        .wave_header = {'W', 'A', 'V', 'E'},
        .fmt_header = {'f', 'm', 't', ' '},
        .fmt_chunk_size = 16,
        .audio_format = 1,
        .num_channels = 1,
        .sample_rate = 16000,
        .byte_rate = 32000,
        .block_align = 2,
        .bits_per_sample = 16,
        .data_header = {'d', 'a', 't', 'a'},
        .data_bytes = data_size
    };
    fseek(f, 0, SEEK_SET);
    fwrite(&header, sizeof(struct WavHeader), 1, f);
}

// Record raw microphone PCM to WAV file
static void record_user_voice(void)
{
    const char *filepath = "/spiffs/record.wav";
    FILE *f = fopen(filepath, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open record.wav for writing");
        oled_set_custom_message("RECORD ERROR", "Unable to open SPIFFS storage");
        oled_set_state(OLED_STATE_ERROR);
        return;
    }

    // Write blank header
    struct WavHeader dummy_hdr = {0};
    fwrite(&dummy_hdr, sizeof(dummy_hdr), 1, f);

    oled_set_state(OLED_STATE_LISTENING);
    audio_record_start();

    int16_t buffer[512];
    size_t total_samples = 0;
    
    ESP_LOGI(TAG, "Recording voice input...");
    
    int duration_ms = 0;
    // Record while button is held down (active low) or 8 seconds limit
    while (gpio_get_level(BOOT_BUTTON_GPIO) == 0 && duration_ms < 8000) {
        size_t samples_read = 0;
        esp_err_t err = audio_record_read(buffer, 512, &samples_read, 100);
        if (err == ESP_OK && samples_read > 0) {
            fwrite(buffer, sizeof(int16_t), samples_read, f);
            total_samples += samples_read;
            
            // Render mic level on OLED
            float rms = audio_calculate_rms(buffer, samples_read);
            oled_set_mic_level(rms);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
        duration_ms += 110;
    }

    audio_record_stop();
    
    int32_t data_bytes = total_samples * sizeof(int16_t);
    write_wav_header(f, data_bytes);
    fclose(f);
    
    char msg[64];
    snprintf(msg, sizeof(msg), "Saved %d KB WAV", (int)(data_bytes / 1024));
    oled_set_custom_message("RECORD SUCCESS", msg);
    ESP_LOGI(TAG, "Recording completed. Captured %d bytes.", data_bytes);
}

void app_main(void)
{
    ESP_LOGI(TAG, "Initializing JARVIS-S3 Phase 5 (Audio Subsystem)...");

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

    // 6. Connect Telegram bot listener
    telegram_init();

    // Update Display state to Ready
    oled_set_state(OLED_STATE_READY);
    ESP_LOGI(TAG, "System Ready! Press and Hold GPIO0 (BOOT button) to record WAV.");

    char query_text[256] = {0};
    while (1) {
        // Poll BOOT button state (0 means pressed)
        if (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
            record_user_voice();
            
            // Set display to thinking during STT processing
            oled_set_state(OLED_STATE_THINKING);
            ESP_LOGI(TAG, "Starting Speech-to-Text transcription...");
            
            esp_err_t err = backend_speech_to_text(NULL, query_text, sizeof(query_text));
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "Transcription result: %s", query_text);
                oled_set_custom_message("YOU SAID", query_text);
            } else {
                ESP_LOGE(TAG, "Transcription failed.");
                oled_set_custom_message("STT ERROR", "Failed to transcribe audio");
                oled_set_state(OLED_STATE_ERROR);
            }
            
            vTaskDelay(pdMS_TO_TICKS(3000));
            oled_set_state(OLED_STATE_READY);
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
