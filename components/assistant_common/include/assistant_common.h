#ifndef ASSISTANT_COMMON_H
#define ASSISTANT_COMMON_H

#include <esp_err.h>
#include <esp_http_client.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// ============================================================================
// Assistant State Machine
// ============================================================================
typedef enum {
    ASSISTANT_STATE_BOOTING,
    ASSISTANT_STATE_IDLE,
    ASSISTANT_STATE_LISTENING,
    ASSISTANT_STATE_STT_PROCESSING,
    ASSISTANT_STATE_AI_PROCESSING,
    ASSISTANT_STATE_TTS_GENERATING,
    ASSISTANT_STATE_SPEAKING,
    ASSISTANT_STATE_ERROR
} assistant_state_t;

// Global state — read by all tasks, written by assistant_task
extern volatile assistant_state_t g_assistant_state;

// Global cancellation flag — set by button ISR task, checked by pipeline
extern volatile bool g_cancel_requested;

// ============================================================================
// Shared WAV Header (eliminates 3 duplicate definitions)
// ============================================================================
struct __attribute__((packed)) WavHeader {
    char riff_header[4];     // "RIFF"
    int32_t wav_size;        // Size of file minus 8
    char wave_header[4];     // "WAVE"
    char fmt_header[4];      // "fmt "
    int32_t fmt_chunk_size;  // 16
    int16_t audio_format;    // 1 (PCM)
    int16_t num_channels;    // 1 (Mono)
    int32_t sample_rate;     // 16000
    int32_t byte_rate;       // sample_rate * num_channels * bits/8
    int16_t block_align;     // num_channels * bits/8
    int16_t bits_per_sample; // 16
    char data_header[4];     // "data"
    int32_t data_bytes;      // Number of bytes in data chunk
};

// ============================================================================
// HTTP Client Registry (for forced cancellation of network requests)
// ============================================================================

/**
 * @brief Register the currently active HTTP client.
 * Called by backend functions before starting network operations.
 * Protected by mutex for thread safety.
 */
void assistant_register_http_client(esp_http_client_handle_t client);

/**
 * @brief Unregister the active HTTP client.
 * Called by backend functions after completing/aborting network operations.
 */
void assistant_unregister_http_client(void);

/**
 * @brief Forcefully abort the active HTTP client connection.
 * Called by the button task to instantly terminate network requests.
 * Thread-safe via mutex.
 */
void assistant_abort_http_client(void);

// ============================================================================
// Cancellation Helper
// ============================================================================

/**
 * @brief Check if cancellation has been requested.
 * Convenience wrapper around g_cancel_requested.
 * @return true if the current operation should be aborted.
 */
bool assistant_is_cancelled(void);

// ============================================================================
// Heap Monitoring
// ============================================================================

/**
 * @brief Log current heap usage for debugging memory leaks.
 * @param tag Context string (e.g., "IDLE", "POST_STT")
 */
void assistant_log_heap(const char *tag);

/**
 * @brief Initialize the assistant common infrastructure.
 * Must be called once during boot before any other assistant_* functions.
 */
void assistant_common_init(void);

#endif // ASSISTANT_COMMON_H
