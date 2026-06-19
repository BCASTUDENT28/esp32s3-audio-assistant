#include "assistant_common.h"
#include <esp_log.h>
#include <esp_system.h>
#include <esp_heap_caps.h>

static const char *TAG = "ASSISTANT";

// ============================================================================
// Global State
// ============================================================================
volatile assistant_state_t g_assistant_state = ASSISTANT_STATE_BOOTING;
volatile bool g_cancel_requested = false;

// ============================================================================
// HTTP Client Registry
// ============================================================================
static esp_http_client_handle_t s_active_http_client = NULL;
static SemaphoreHandle_t s_http_mutex = NULL;

void assistant_common_init(void)
{
    s_http_mutex = xSemaphoreCreateMutex();
    configASSERT(s_http_mutex);
    ESP_LOGI(TAG, "Assistant common infrastructure initialized.");
}

void assistant_register_http_client(esp_http_client_handle_t client)
{
    if (xSemaphoreTake(s_http_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_active_http_client = client;
        xSemaphoreGive(s_http_mutex);
    }
}

void assistant_unregister_http_client(void)
{
    if (xSemaphoreTake(s_http_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_active_http_client = NULL;
        xSemaphoreGive(s_http_mutex);
    }
}

void assistant_abort_http_client(void)
{
    if (xSemaphoreTake(s_http_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        if (s_active_http_client != NULL) {
            ESP_LOGW(TAG, "Force-aborting active HTTP client connection.");
            esp_http_client_close(s_active_http_client);
            s_active_http_client = NULL;
        }
        xSemaphoreGive(s_http_mutex);
    }
}

bool assistant_is_cancelled(void)
{
    return g_cancel_requested;
}

void assistant_log_heap(const char *tag)
{
    ESP_LOGI(TAG, "[HEAP][%s] Free: %lu bytes | Internal: %lu bytes | Min-ever: %lu bytes",
             tag,
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned long)esp_get_minimum_free_heap_size());
}
