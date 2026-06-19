#include "audio_io.h"
#include <esp_log.h>
#include <driver/i2s_std.h>
#include <driver/i2s_pdm.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <math.h>
#include <string.h>

static const char *TAG = "AUDIO_IO";

// I2S RX Pins (INMP441 Microphone)
#define I2S_RX_BCLK_IO      GPIO_NUM_41
#define I2S_RX_WS_IO        GPIO_NUM_42
#define I2S_RX_DIN_IO       GPIO_NUM_2

// I2S TX Pins (DAC feeding PAM8403)
#define I2S_TX_BCLK_IO      GPIO_NUM_18
#define I2S_TX_WS_IO        GPIO_NUM_17
#define I2S_TX_DOUT_IO      GPIO_NUM_16

static i2s_chan_handle_t tx_handle = NULL;
static i2s_chan_handle_t rx_handle = NULL;
static bool rx_active = false;
static bool tx_active = false;

// Buffer size for dynamic raw data reads
#define RAW_RX_BUF_SIZE     1024

esp_err_t audio_io_init(void)
{
    ESP_LOGI(TAG, "Initializing I2S audio drivers...");

    // ----------------------------------------------------
    // 1. Configure I2S RX Channel (Microphone) on I2S_NUM_1
    // ----------------------------------------------------
    i2s_chan_config_t rx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    esp_err_t err = i2s_new_channel(&rx_chan_cfg, NULL, &rx_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2S RX channel: %s", esp_err_to_name(err));
        return err;
    }

    i2s_std_config_t rx_std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000), // 16kHz
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_RX_BCLK_IO,
            .ws = I2S_RX_WS_IO,
            .dout = I2S_GPIO_UNUSED,
            .din = I2S_RX_DIN_IO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    rx_std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT; // Microphone typically outputs Left

    err = i2s_channel_init_std_mode(rx_handle, &rx_std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize I2S RX channel: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "I2S RX (INMP441) initialized on GPIO BCLK:%d, WS:%d, DIN:%d", I2S_RX_BCLK_IO, I2S_RX_WS_IO, I2S_RX_DIN_IO);

    // ----------------------------------------------------
    // 2. Configure I2S TX Channel (Speaker/DAC) on I2S_NUM_0
    // ----------------------------------------------------
    i2s_chan_config_t tx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    err = i2s_new_channel(&tx_chan_cfg, &tx_handle, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2S TX channel: %s", esp_err_to_name(err));
        return err;
    }

    i2s_std_config_t tx_std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000), // 16kHz
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_TX_BCLK_IO,
            .ws = I2S_TX_WS_IO,
            .dout = I2S_TX_DOUT_IO,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    tx_std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;

    err = i2s_channel_init_std_mode(tx_handle, &tx_std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize I2S Standard TX channel: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "I2S Standard TX initialized on GPIO BCLK:%d, WS:%d, DOUT:%d", I2S_TX_BCLK_IO, I2S_TX_WS_IO, I2S_TX_DOUT_IO);

    return ESP_OK;
}

esp_err_t audio_record_start(void)
{
    if (rx_active) return ESP_OK;
    esp_err_t err = i2s_channel_enable(rx_handle);
    if (err == ESP_OK) {
        rx_active = true;
        ESP_LOGD(TAG, "I2S RX enabled.");
    }
    return err;
}

esp_err_t audio_record_read(int16_t *out_pcm16, size_t samples_requested, size_t *samples_read, uint32_t timeout_ms)
{
    if (!rx_active) return ESP_ERR_INVALID_STATE;

    size_t total_samples_read = 0;
    // We allocate a temporary buffer to read the raw 32-bit I2S samples
    int32_t raw_buffer[RAW_RX_BUF_SIZE];
    size_t chunk_size = RAW_RX_BUF_SIZE;

    while (total_samples_read < samples_requested) {
        size_t samples_to_read = samples_requested - total_samples_read;
        if (samples_to_read > chunk_size) {
            samples_to_read = chunk_size;
        }

        size_t bytes_read = 0;
        esp_err_t err = i2s_channel_read(rx_handle, raw_buffer, samples_to_read * sizeof(int32_t), &bytes_read, pdMS_TO_TICKS(timeout_ms));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "I2S read error: %s", esp_err_to_name(err));
            return err;
        }

        size_t read_count = bytes_read / sizeof(int32_t);
        if (read_count == 0) {
            break; // Timeout or no data
        }

        // Convert 32-bit raw audio samples from INMP441 to 16-bit PCM.
        // We shift by 14 bits instead of 16 to provide a clean digital amplification gain of 4x.
        const float gain = 1.5f;
        for (size_t i = 0; i < read_count; i++) {
            // INMP441 outputs 24-bit MSB-aligned in a 32-bit slot
            int32_t val32 = raw_buffer[i];
            
            // Shift right to get 16-bit range, apply gain, and clip
            int32_t amplified = (int32_t)((val32 >> 14) * gain);
            
            if (amplified > 32767) {
                amplified = 32767;
            } else if (amplified < -32768) {
                amplified = -32768;
            }
            
            out_pcm16[total_samples_read + i] = (int16_t)amplified;
        }

        total_samples_read += read_count;
    }

    *samples_read = total_samples_read;
    return ESP_OK;
}

esp_err_t audio_record_stop(void)
{
    if (!rx_active) return ESP_OK;
    esp_err_t err = i2s_channel_disable(rx_handle);
    if (err == ESP_OK) {
        rx_active = false;
        ESP_LOGD(TAG, "I2S RX disabled.");
    }
    return err;
}

esp_err_t audio_play_start(void)
{
    if (tx_active) return ESP_OK;
    esp_err_t err = i2s_channel_enable(tx_handle);
    if (err == ESP_OK) {
        tx_active = true;
        ESP_LOGD(TAG, "I2S TX enabled.");
    }
    return err;
}

esp_err_t audio_play_write(const int16_t *pcm16, size_t samples_count, size_t *samples_written, uint32_t timeout_ms)
{
    if (!tx_active) return ESP_ERR_INVALID_STATE;

    // We output stereo, duplicating the mono samples for L & R channels.
    // Allocate a temporary buffer for stereo PCM16.
    size_t chunk_size = 512;
    int16_t stereo_buffer[1024]; // 512 * 2 samples
    size_t total_samples_written = 0;

    while (total_samples_written < samples_count) {
        size_t chunk = samples_count - total_samples_written;
        if (chunk > chunk_size) {
            chunk = chunk_size;
        }

        // Populate stereo buffer: duplicate left channel sample to right channel slot
        for (size_t i = 0; i < chunk; i++) {
            int16_t val = pcm16[total_samples_written + i];
            stereo_buffer[i * 2]     = val; // Left channel
            stereo_buffer[i * 2 + 1] = val; // Right channel
        }

        size_t bytes_written = 0;
        esp_err_t err = i2s_channel_write(tx_handle, stereo_buffer, chunk * 2 * sizeof(int16_t), &bytes_written, pdMS_TO_TICKS(timeout_ms));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "I2S write error: %s", esp_err_to_name(err));
            return err;
        }

        size_t written_count = bytes_written / (2 * sizeof(int16_t));
        if (written_count == 0) {
            break; // Timeout or full
        }

        total_samples_written += written_count;
    }

    *samples_written = total_samples_written;
    return ESP_OK;
}

esp_err_t audio_play_stop(void)
{
    if (!tx_active) return ESP_OK;
    esp_err_t err = i2s_channel_disable(tx_handle);
    if (err == ESP_OK) {
        tx_active = false;
        ESP_LOGD(TAG, "I2S TX disabled.");
    }
    return err;
}

esp_err_t audio_play_set_sample_rate(uint32_t sample_rate)
{
    if (!tx_handle) return ESP_ERR_INVALID_STATE;
    ESP_LOGI(TAG, "Reconfiguring I2S Standard TX sample rate to %lu Hz", (unsigned long)sample_rate);
    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
    return i2s_channel_reconfig_std_clock(tx_handle, &clk_cfg);
}

float audio_calculate_rms(const int16_t *samples, size_t num_samples)
{
    if (num_samples == 0) return 0.0f;
    
    double sum = 0.0;
    for (size_t i = 0; i < num_samples; i++) {
        double val = (double)samples[i] / 32768.0;
        sum += val * val;
    }
    
    float rms = sqrtf((float)(sum / num_samples));
    return rms;
}

void audio_force_stop_all(void)
{
    ESP_LOGW(TAG, "Force-stopping all audio channels.");
    if (tx_active) {
        i2s_channel_disable(tx_handle);
        tx_active = false;
    }
    if (rx_active) {
        i2s_channel_disable(rx_handle);
        rx_active = false;
    }
}

esp_err_t audio_transition_to_recording(void)
{
    ESP_LOGI(TAG, "Transitioning to RECORDING mode.");
    // Stop speaker first
    if (tx_active) {
        i2s_channel_disable(tx_handle);
        tx_active = false;
    }
    // Start microphone
    if (!rx_active) {
        esp_err_t err = i2s_channel_enable(rx_handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to enable RX channel: %s", esp_err_to_name(err));
            return err;
        }
        rx_active = true;
    }
    return ESP_OK;
}

esp_err_t audio_transition_to_playback(void)
{
    ESP_LOGI(TAG, "Transitioning to PLAYBACK mode.");
    // Stop microphone first
    if (rx_active) {
        i2s_channel_disable(rx_handle);
        rx_active = false;
    }
    // Start speaker
    if (!tx_active) {
        esp_err_t err = i2s_channel_enable(tx_handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to enable TX channel: %s", esp_err_to_name(err));
            return err;
        }
        tx_active = true;
    }
    return ESP_OK;
}

void audio_play_listen_beep(int count)
{
    // Each beep: 100ms of 880Hz sine wave, 80ms silence between beeps
    const int sample_rate = 16000;
    const int beep_samples = sample_rate / 10;  // 100ms = 1600 samples
    const int silence_samples = sample_rate * 80 / 1000;  // 80ms
    
    audio_play_set_sample_rate(sample_rate);
    audio_transition_to_playback();
    
    int16_t *buf = malloc(256 * sizeof(int16_t));
    if (!buf) return;
    
    for (int b = 0; b < count; b++) {
        // Play beep
        size_t written = 0;
        for (int i = 0; i < beep_samples; i += 256) {
            int chunk = (beep_samples - i > 256) ? 256 : (beep_samples - i);
            for (int j = 0; j < chunk; j++) {
                float t = (float)(i + j) / sample_rate;
                buf[j] = (int16_t)(sinf(2.0f * M_PI * 880.0f * t) * 8000.0f);
            }
            audio_play_write(buf, chunk, &written, 100);
        }
        
        // Silence between beeps (if more beeps follow)
        if (b < count - 1) {
            memset(buf, 0, 256 * sizeof(int16_t));
            for (int i = 0; i < silence_samples; i += 256) {
                int chunk = (silence_samples - i > 256) ? 256 : (silence_samples - i);
                audio_play_write(buf, chunk, &written, 100);
            }
        }
    }
    
    // Small tail silence to flush DMA
    memset(buf, 0, 256 * sizeof(int16_t));
    size_t w = 0;
    audio_play_write(buf, 256, &w, 100);
    
    free(buf);
    audio_play_stop();
}

void audio_play_error_tone(void)
{
    // Descending tone: 660Hz -> 440Hz -> 330Hz, 100ms each
    const int sample_rate = 16000;
    const int tone_samples = sample_rate / 10;  // 100ms
    const float freqs[] = {660.0f, 440.0f, 330.0f};
    
    audio_play_set_sample_rate(sample_rate);
    audio_transition_to_playback();
    
    int16_t *buf = malloc(256 * sizeof(int16_t));
    if (!buf) return;
    
    for (int f = 0; f < 3; f++) {
        size_t written = 0;
        for (int i = 0; i < tone_samples; i += 256) {
            int chunk = (tone_samples - i > 256) ? 256 : (tone_samples - i);
            for (int j = 0; j < chunk; j++) {
                float t = (float)(i + j) / sample_rate;
                buf[j] = (int16_t)(sinf(2.0f * M_PI * freqs[f] * t) * 6000.0f);
            }
            audio_play_write(buf, chunk, &written, 100);
        }
    }
    
    free(buf);
    audio_play_stop();
}
