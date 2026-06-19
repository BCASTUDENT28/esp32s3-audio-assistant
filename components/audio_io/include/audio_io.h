#ifndef AUDIO_IO_H
#define AUDIO_IO_H

#include <esp_err.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @brief Initialize the I2S hardware.
 * Configures I2S_NUM_0 for TX (speaker/DAC) and I2S_NUM_1 for RX (microphone).
 * @return esp_err_t 
 */
esp_err_t audio_io_init(void);

/**
 * @brief Enable the microphone I2S RX channel.
 */
esp_err_t audio_record_start(void);

/**
 * @brief Read audio data from the microphone.
 * Automatically downsamples/converts the raw I2S data to 16-bit PCM, 16kHz mono.
 * 
 * @param out_pcm16 Pointer to buffer where 16-bit PCM samples will be written.
 * @param samples_requested Number of samples to read.
 * @param samples_read Pointer to store how many samples were successfully read.
 * @param timeout_ms Timeout in milliseconds.
 * @return esp_err_t 
 */
esp_err_t audio_record_read(int16_t *out_pcm16, size_t samples_requested, size_t *samples_read, uint32_t timeout_ms);

/**
 * @brief Disable the microphone I2S RX channel.
 */
esp_err_t audio_record_stop(void);

/**
 * @brief Enable the speaker I2S TX channel.
 */
esp_err_t audio_play_start(void);

/**
 * @brief Play audio data to the speaker/DAC.
 * Accepts 16-bit PCM, 16kHz mono audio data and writes to the TX buffer.
 * 
 * @param pcm16 Pointer to 16-bit PCM source buffer.
 * @param samples_count Number of samples to write.
 * @param samples_written Pointer to store how many samples were successfully written.
 * @param timeout_ms Timeout in milliseconds.
 * @return esp_err_t 
 */
esp_err_t audio_play_write(const int16_t *pcm16, size_t samples_count, size_t *samples_written, uint32_t timeout_ms);

/**
 * @brief Disable the speaker I2S TX channel.
 */
esp_err_t audio_play_stop(void);

/**
 * @brief Dynamically reconfigure the I2S TX sample rate.
 * 
 * @param sample_rate Sample rate in Hz (e.g., 16000, 24000).
 * @return esp_err_t 
 */
esp_err_t audio_play_set_sample_rate(uint32_t sample_rate);

/**
 * @brief Calculate the Root Mean Square (RMS) level of a buffer of audio samples.
 * Used for dynamic volume visualization (e.g. on the OLED).
 * 
 * @param samples Pointer to PCM 16-bit samples.
 * @param num_samples Number of samples.
 * @return float Normalized value between 0.0 (silent) and 1.0 (clipping).
 */
float audio_calculate_rms(const int16_t *samples, size_t num_samples);
/**
 * @brief Force-stop all audio activity (both speaker and microphone).
 * Safe to call from any state. Resets both rx_active and tx_active.
 */
void audio_force_stop_all(void);

/**
 * @brief Safely transition to recording mode.
 * Stops the speaker first, then starts the microphone.
 * @return esp_err_t
 */
esp_err_t audio_transition_to_recording(void);

/**
 * @brief Safely transition to playback mode.
 * Stops the microphone first, then starts the speaker.
 * @return esp_err_t
 */
esp_err_t audio_transition_to_playback(void);

/**
 * @brief Play a short listening beep (~100ms, 880Hz).
 * Uses the I2S speaker. Caller should ensure speaker is available.
 * @param count Number of beeps (1 = listening, 2 = interrupted)
 */
void audio_play_listen_beep(int count);

/**
 * @brief Play an error tone (~300ms descending).
 */
void audio_play_error_tone(void);

#endif // AUDIO_IO_H
