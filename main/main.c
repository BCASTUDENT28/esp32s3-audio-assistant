#include <stdio.h>
#include <string.h>
#include <math.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <esp_log.h>
#include <esp_system.h>
#include <driver/gpio.h>

#include "assistant_common.h"
#include "storage.h"
#include "oled_display.h"
#include "wifi_manager.h"
#include "backend_client.h"
#include "telegram.h"
#include "audio_io.h"
#include "memory_manager.h"
#include "home_automation.h"

static const char *TAG = "MAIN";

// ============================================================================
// Configuration
// ============================================================================
#define BOOT_BUTTON_GPIO        GPIO_NUM_0
#define VAD_SILENCE_THRESHOLD   0.008f    // RMS below this = silence
#define VAD_SILENCE_TIMEOUT_MS  1800      // 1.8 seconds of silence = stop recording
#define MAX_RECORD_DURATION_MS  15000     // Maximum recording = 15 seconds
#define RECORD_CHUNK_SAMPLES    512

// ============================================================================
// Task Handles
// ============================================================================
static TaskHandle_t s_button_task_handle = NULL;
static TaskHandle_t s_assistant_task_handle = NULL;

// ============================================================================
// Shared WAV Header Helper
// ============================================================================
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

// ============================================================================
// GPIO ISR Handler (runs in IRAM, must be fast)
// ============================================================================
static void IRAM_ATTR boot_button_isr_handler(void *arg)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    vTaskNotifyGiveFromISR(s_button_task_handle, &xHigherPriorityTaskWoken);
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

// ============================================================================
// Button Task — Handles debounce, cancellation, and assistant notification
// ============================================================================
static void button_task(void *pvParameters)
{
    ESP_LOGI(TAG, "[BOOT] Button task started.");
    
    while (1) {
        // Sleep until ISR fires
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        
        // Debounce: wait 50ms, then re-check button state
        vTaskDelay(pdMS_TO_TICKS(50));
        if (gpio_get_level(BOOT_BUTTON_GPIO) != 0) {
            continue;  // False trigger or bounce
        }
        
        ESP_LOGW(TAG, "[BOOT] Interrupt received — current state: %d", g_assistant_state);
        
        // 1. Set the global cancellation flag
        g_cancel_requested = true;
        
        // 2. Force-abort any active HTTP request (breaks network blocking)
        assistant_abort_http_client();
        
        // 3. Force-stop all audio (speaker + mic)
        audio_force_stop_all();
        ESP_LOGI(TAG, "[AUDIO] Playback stopped");
        
        // 4. Signal the assistant task to wake up and enter LISTENING
        xTaskNotifyGive(s_assistant_task_handle);
        
        // Wait for button release before accepting another press
        while (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        vTaskDelay(pdMS_TO_TICKS(100));  // Extra debounce after release
    }
}

// ============================================================================
// Record User Voice with VAD (Voice Activity Detection)
//   - Records until 1.8s of silence or 15s max
//   - Returns true if meaningful audio was captured
// ============================================================================
static bool record_user_voice(void)
{
    const char *filepath = "/spiffs/record.wav";
    FILE *f = fopen(filepath, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open record.wav for writing");
        return false;
    }

    // Write blank header placeholder
    struct WavHeader dummy_hdr = {0};
    fwrite(&dummy_hdr, sizeof(dummy_hdr), 1, f);

    int16_t buffer[RECORD_CHUNK_SAMPLES];
    size_t total_samples = 0;
    int silence_ms = 0;
    int total_ms = 0;
    bool had_voice = false;

    ESP_LOGI(TAG, "[MIC] Recording started (VAD mode, silence timeout: %dms)", VAD_SILENCE_TIMEOUT_MS);

    while (total_ms < MAX_RECORD_DURATION_MS) {
        // Check for re-interrupt
        if (assistant_is_cancelled()) {
            ESP_LOGW(TAG, "[MIC] Recording cancelled by user.");
            fclose(f);
            return false;
        }

        size_t samples_read = 0;
        esp_err_t err = audio_record_read(buffer, RECORD_CHUNK_SAMPLES, &samples_read, 100);
        if (err != ESP_OK || samples_read == 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
            total_ms += 10;
            continue;
        }

        // Write audio data to file
        fwrite(buffer, sizeof(int16_t), samples_read, f);
        total_samples += samples_read;

        // Calculate RMS for VAD and OLED visualization
        float rms = audio_calculate_rms(buffer, samples_read);
        oled_set_mic_level(rms);

        // Voice Activity Detection
        int chunk_duration_ms = (samples_read * 1000) / 16000;
        total_ms += chunk_duration_ms;

        if (rms > VAD_SILENCE_THRESHOLD) {
            // Voice detected
            had_voice = true;
            silence_ms = 0;
        } else {
            // Silence detected
            silence_ms += chunk_duration_ms;
        }

        // Stop recording after sustained silence (but only if we heard voice first)
        if (had_voice && silence_ms >= VAD_SILENCE_TIMEOUT_MS) {
            ESP_LOGI(TAG, "[MIC] VAD: Silence detected for %dms. Stopping.", silence_ms);
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(5));
    }

    audio_record_stop();

    int32_t data_bytes = total_samples * sizeof(int16_t);
    write_wav_header(f, data_bytes);
    fclose(f);

    if (data_bytes < 3200) {  // Less than 0.1 seconds of audio
        ESP_LOGW(TAG, "[MIC] Recording too short (%d bytes). Discarding.", data_bytes);
        return false;
    }

    ESP_LOGI(TAG, "[MIC] Recording complete. %d bytes (%.1f seconds)", 
             data_bytes, (float)total_samples / 16000.0f);
    return true;
}

// ============================================================================
// Play TTS Response with Interrupt Support
// ============================================================================
static void play_tts_response(void)
{
    const char *filepath = "/spiffs/tts.wav";
    FILE *f = fopen(filepath, "rb");
    if (!f) {
        ESP_LOGE(TAG, "[TTS] Failed to open tts.wav for playing");
        return;
    }

    // Read and validate WAV header
    struct WavHeader header;
    if (fread(&header, 1, sizeof(struct WavHeader), f) != sizeof(struct WavHeader)) {
        ESP_LOGE(TAG, "[TTS] Failed to read WAV header");
        fclose(f);
        return;
    }

    if (memcmp(header.riff_header, "RIFF", 4) != 0 || memcmp(header.wave_header, "WAVE", 4) != 0) {
        ESP_LOGE(TAG, "[TTS] Invalid WAV file");
        fclose(f);
        return;
    }

    uint32_t sample_rate = header.sample_rate;
    ESP_LOGI(TAG, "[TTS] Speaking: rate=%luHz, bits=%d, ch=%d", 
             (unsigned long)sample_rate, header.bits_per_sample, header.num_channels);

    // Reconfigure speaker sample rate and transition to playback
    audio_play_set_sample_rate(sample_rate);
    audio_transition_to_playback();

    int16_t buffer[512];
    size_t bytes_read;
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), f)) > 0) {
        // Check for interrupt during playback
        if (assistant_is_cancelled()) {
            ESP_LOGW(TAG, "[AUDIO] Playback interrupted by user.");
            break;
        }

        size_t samples_count = bytes_read / sizeof(int16_t);
        size_t samples_written = 0;
        esp_err_t err = audio_play_write(buffer, samples_count, &samples_written, 100);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "[TTS] Playback write error: %s", esp_err_to_name(err));
            break;
        }
    }

    audio_play_stop();
    fclose(f);
    ESP_LOGI(TAG, "[TTS] Playback finished.");
}

// ============================================================================
// Parse Home Automation Commands from AI Response
// ============================================================================
static void parse_home_automation_commands(char *reply_text)
{
    if (strstr(reply_text, "[RELAY_ON]")) {
        home_auto_set_relay(true);
        char *p = strstr(reply_text, "[RELAY_ON]");
        memset(p, ' ', 10);
    } else if (strstr(reply_text, "[RELAY_OFF]")) {
        home_auto_set_relay(false);
        char *p = strstr(reply_text, "[RELAY_OFF]");
        memset(p, ' ', 11);
    } else if (strstr(reply_text, "[SERVO_OPEN]")) {
        home_auto_set_servo_angle(90);
        char *p = strstr(reply_text, "[SERVO_OPEN]");
        memset(p, ' ', 12);
    } else if (strstr(reply_text, "[SERVO_CLOSE]")) {
        home_auto_set_servo_angle(0);
        char *p = strstr(reply_text, "[SERVO_CLOSE]");
        memset(p, ' ', 13);
    }
}

// ============================================================================
// Error Handler — Shows error on OLED, plays error tone, auto-recovers
// ============================================================================
static void handle_error(const char *error_msg)
{
    ESP_LOGE(TAG, "[ERROR] %s", error_msg);
    g_assistant_state = ASSISTANT_STATE_ERROR;
    oled_set_custom_message("ERROR", error_msg);
    oled_set_state(OLED_STATE_ERROR);
    audio_play_error_tone();
    vTaskDelay(pdMS_TO_TICKS(2000));
    // Auto-recover to IDLE
    g_assistant_state = ASSISTANT_STATE_IDLE;
    oled_set_state(OLED_STATE_READY);
}

// ============================================================================
// Assistant Task — Main Voice Pipeline State Machine
// ============================================================================
static void assistant_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Assistant task started. Waiting for button press...");

    // Check if OpenAI key exists
    char openai_check[192] = {0};
    bool has_openai_key = (storage_read_string("openai_key", openai_check, sizeof(openai_check)) == ESP_OK 
                           && strlen(openai_check) > 0);
    
    if (!has_openai_key) {
        ESP_LOGW(TAG, "No 'openai_key' in NVS. Voice pipeline disabled.");
        ESP_LOGW(TAG, "Telegram AI chat via DeepSeek is still operational.");
    } else {
        ESP_LOGI(TAG, "OpenAI key found. Voice pipeline (STT + TTS) enabled.");
    }

    g_assistant_state = ASSISTANT_STATE_IDLE;
    oled_set_state(OLED_STATE_READY);
    assistant_log_heap("BOOT_COMPLETE");

    char query_text[256] = {0};
    char reply_text[256] = {0};

    while (1) {
        // ---- Wait for button notification ----
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        
        // Clear cancellation flag for this new interaction
        g_cancel_requested = false;

        // Check for OpenAI key
        if (!has_openai_key) {
            // Re-check in case user configured via web portal
            has_openai_key = (storage_read_string("openai_key", openai_check, sizeof(openai_check)) == ESP_OK 
                              && strlen(openai_check) > 0);
            if (!has_openai_key) {
                ESP_LOGW(TAG, "[BOOT] No OpenAI key. Voice disabled.");
                oled_set_custom_message("NO API KEY", "Set OpenAI key via web portal");
                oled_set_state(OLED_STATE_ERROR);
                vTaskDelay(pdMS_TO_TICKS(3000));
                oled_set_state(OLED_STATE_READY);
                g_assistant_state = ASSISTANT_STATE_IDLE;
                continue;
            }
        }

        // ============================================================
        // STATE: LISTENING
        // ============================================================
        g_assistant_state = ASSISTANT_STATE_LISTENING;
        oled_set_state(OLED_STATE_LISTENING);

        // Play appropriate beep
        // (Double beep if we interrupted a previous response, single otherwise)
        bool was_interrupted = (g_assistant_state == ASSISTANT_STATE_SPEAKING || 
                                g_assistant_state == ASSISTANT_STATE_TTS_GENERATING ||
                                g_assistant_state == ASSISTANT_STATE_AI_PROCESSING ||
                                g_assistant_state == ASSISTANT_STATE_STT_PROCESSING);
        // Note: state was already set to LISTENING above, so check the cancel context
        audio_play_listen_beep(was_interrupted ? 2 : 1);
        
        ESP_LOGI(TAG, "[MIC] Recording started");

        // Transition to recording (stops speaker, starts mic)
        audio_transition_to_recording();

        // Record with VAD
        memset(query_text, 0, sizeof(query_text));
        memset(reply_text, 0, sizeof(reply_text));
        
        bool got_audio = record_user_voice();
        
        if (assistant_is_cancelled()) {
            ESP_LOGW(TAG, "Cancelled during recording. Restarting...");
            continue;  // Will re-enter from the top with new notification
        }
        if (!got_audio) {
            ESP_LOGW(TAG, "No meaningful audio captured. Returning to IDLE.");
            g_assistant_state = ASSISTANT_STATE_IDLE;
            oled_set_state(OLED_STATE_READY);
            continue;
        }

        // ============================================================
        // STATE: STT_PROCESSING
        // ============================================================
        g_assistant_state = ASSISTANT_STATE_STT_PROCESSING;
        oled_set_state(OLED_STATE_TRANSCRIBING);
        ESP_LOGI(TAG, "[STT] Processing");

        esp_err_t err = backend_speech_to_text(NULL, query_text, sizeof(query_text));
        
        if (assistant_is_cancelled()) {
            ESP_LOGW(TAG, "Cancelled during STT. Restarting...");
            continue;
        }
        if (err != ESP_OK) {
            handle_error("Speech recognition failed");
            continue;
        }

        ESP_LOGI(TAG, "[STT] Result: \"%s\"", query_text);

        // ============================================================
        // STATE: AI_PROCESSING
        // ============================================================
        g_assistant_state = ASSISTANT_STATE_AI_PROCESSING;
        oled_set_state(OLED_STATE_THINKING);
        ESP_LOGI(TAG, "[AI] Sending to DeepSeek...");

        err = backend_deepseek_chat(NULL, query_text, reply_text, sizeof(reply_text));
        
        if (assistant_is_cancelled()) {
            ESP_LOGW(TAG, "Cancelled during AI processing. Restarting...");
            continue;
        }
        if (err != ESP_OK) {
            handle_error("AI response failed");
            continue;
        }

        ESP_LOGI(TAG, "[AI] Response received: \"%s\"", reply_text);

        // Parse and execute home automation commands
        parse_home_automation_commands(reply_text);

        // ============================================================
        // STATE: TTS_GENERATING
        // ============================================================
        g_assistant_state = ASSISTANT_STATE_TTS_GENERATING;
        oled_set_state(OLED_STATE_GENERATING_VOICE);
        ESP_LOGI(TAG, "[TTS] Generating speech...");

        err = backend_text_to_speech(NULL, reply_text, "/spiffs/tts.wav");
        
        if (assistant_is_cancelled()) {
            ESP_LOGW(TAG, "Cancelled during TTS generation. Restarting...");
            continue;
        }
        if (err != ESP_OK) {
            handle_error("Voice generation failed");
            continue;
        }

        // ============================================================
        // STATE: SPEAKING
        // ============================================================
        g_assistant_state = ASSISTANT_STATE_SPEAKING;
        oled_set_state(OLED_STATE_SPEAKING);
        ESP_LOGI(TAG, "[TTS] Speaking");

        play_tts_response();

        // ============================================================
        // STATE: IDLE (return to ready)
        // ============================================================
        if (!assistant_is_cancelled()) {
            g_assistant_state = ASSISTANT_STATE_IDLE;
            oled_set_state(OLED_STATE_READY);
        }

        assistant_log_heap("CYCLE_COMPLETE");
    }
}

// ============================================================================
// app_main — Initialization Only
// ============================================================================
void app_main(void)
{
    ESP_LOGI(TAG, "====================================");
    ESP_LOGI(TAG, "  JARVIS-S3 Voice Assistant v2.0");
    ESP_LOGI(TAG, "  Push-To-Talk State Machine");
    ESP_LOGI(TAG, "====================================");

    g_assistant_state = ASSISTANT_STATE_BOOTING;

    // 1. Initialize Display
    if (oled_init() == ESP_OK) {
        oled_set_state(OLED_STATE_BOOT);
    }
    vTaskDelay(pdMS_TO_TICKS(500));

    // 2. Initialize Secure Storage & SPIFFS
    if (storage_init() != ESP_OK) {
        ESP_LOGE(TAG, "Storage initialization failed.");
        oled_set_custom_message("STORAGE CRITICAL", "NVS/SPIFFS init failed");
        oled_set_state(OLED_STATE_ERROR);
        return;
    }

    // 3. Initialize shared infrastructure (mutexes, etc.)
    assistant_common_init();

    // 4. Initialize Memory Manager
    memory_manager_init();

    // 5. Initialize Home Automation peripherals
    home_auto_init();

    // 6. Initialize Audio I2S drivers
    if (audio_io_init() != ESP_OK) {
        ESP_LOGE(TAG, "Audio hardware initialization failed.");
        oled_set_custom_message("AUDIO ERROR", "I2S init failed");
        oled_set_state(OLED_STATE_ERROR);
    }

    // 7. Configure GPIO0 interrupt (falling edge)
    gpio_config_t btn_cfg = {
        .pin_bit_mask = (1ULL << BOOT_BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,  // Interrupt on falling edge (button press)
    };
    gpio_config(&btn_cfg);

    // 8. Create Button Task (highest priority for instant response)
    xTaskCreatePinnedToCore(
        button_task,
        "button_task",
        2048,               // Stack: 2KB (lightweight)
        NULL,
        10,                 // Priority: 10 (highest — always preempts)
        &s_button_task_handle,
        0                   // Core 0
    );

    // 9. Install GPIO ISR service and attach handler
    gpio_install_isr_service(0);
    gpio_isr_handler_add(BOOT_BUTTON_GPIO, boot_button_isr_handler, NULL);
    ESP_LOGI(TAG, "GPIO0 ISR installed. Button task ready.");

    // 10. Connect WiFi or start captive configuration portal
    oled_set_state(OLED_STATE_WIFI_CONNECTING);
    esp_err_t wifi_err = wifi_manager_init(true);
    if (wifi_err != ESP_OK || !wifi_manager_is_connected()) {
        ESP_LOGI(TAG, "WiFi Onboarding AP Portal active. Configure via http://192.168.4.1");
        oled_set_state(OLED_STATE_WIFI_PORTAL);
        while (1) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    // 11. Connect Telegram bot listener
    telegram_init();

    // 12. Create Assistant Task (voice pipeline state machine)
    xTaskCreatePinnedToCore(
        assistant_task,
        "assistant_task",
        8192,               // Stack: 8KB (handles file I/O + JSON)
        NULL,
        5,                  // Priority: 5 (normal)
        &s_assistant_task_handle,
        0                   // Core 0
    );

    ESP_LOGI(TAG, "System Ready! Press BOOT button to talk to Jarvis.");
    assistant_log_heap("SYSTEM_INIT");

    // app_main() returns — FreeRTOS scheduler takes over
}
