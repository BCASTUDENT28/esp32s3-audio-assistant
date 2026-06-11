#include "telegram.h"
#include "storage.h"
#include "ota.h"
#include "memory_manager.h"

#include <string.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#include <cJSON.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char *TAG = "TELEGRAM";

#define MAX_HTTP_RECV_BUFFER 4096

// Structures for HTTP responses
struct HttpBuffer {
    char *data;
    int index;
    int limit;
};

// HTTP Client Event Handler
static esp_err_t http_event_handler(esp_http_client_event_handle_t evt)
{
    struct HttpBuffer *buf = (struct HttpBuffer *)evt->user_data;
    switch(evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (buf && buf->data && (buf->index + evt->data_len < buf->limit)) {
                memcpy(buf->data + buf->index, evt->data, evt->data_len);
                buf->index += evt->data_len;
                buf->data[buf->index] = '\0';
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

// Send telegram text message
static void telegram_send_message(const char *token, int64_t chat_id, const char *text)
{
    char url[256];
    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/sendMessage", token);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "chat_id", chat_id);
    cJSON_AddStringToObject(root, "text", text);
    char *post_data = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 5000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client) {
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_err_t err = esp_http_client_open(client, strlen(post_data));
        if (err == ESP_OK) {
            esp_http_client_write(client, post_data, strlen(post_data));
            esp_http_client_fetch_headers(client);
        }
        esp_http_client_cleanup(client);
    }
    free(post_data);
}

// Process single bot message
static void handle_telegram_message(const char *token, int64_t chat_id, const char *text)
{
    ESP_LOGI(TAG, "Received Telegram command: %s", text);

    if (strcmp(text, "/status") == 0) {
        char reply[256];
        uint32_t free_heap = esp_get_free_heap_size();
        uint32_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        
        char ssid[32] = {0};
        storage_read_string("wifi_ssid", ssid, sizeof(ssid));
        
        snprintf(reply, sizeof(reply),
                 "[BOT] JARVIS-S3 Voice Assistant Status:\n\n"
                 "WiFi Network: %s\n"
                 "Free Internal Heap: %d KB\n"
                 "Free PSRAM: %d KB\n"
                 "System Status: ACTIVE",
                 ssid, (int)(free_heap / 1024), (int)(free_psram / 1024));
        telegram_send_message(token, chat_id, reply);
    } 
    else if (strcmp(text, "/reboot") == 0) {
        telegram_send_message(token, chat_id, "Rebooting device...");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    } 
    else if (strcmp(text, "/clear") == 0) {
        telegram_send_message(token, chat_id, "Clearing configuration and conversation history, resetting to AP mode...");
        memory_clear_history();
        vTaskDelay(pdMS_TO_TICKS(1000));
        storage_clear_all();
        esp_restart();
    } 
    else if (strcmp(text, "/clear_history") == 0) {
        memory_clear_history();
        telegram_send_message(token, chat_id, "Conversation memory cleared successfully.");
    }
    else if (strncmp(text, "/ota ", 5) == 0) {
        const char *ota_url = text + 5;
        char reply[256];
        snprintf(reply, sizeof(reply), "Starting OTA firmware update from:\n%s", ota_url);
        telegram_send_message(token, chat_id, reply);
        
        esp_err_t err = ota_start(ota_url);
        if (err != ESP_OK) {
            char fail_reply[128];
            snprintf(fail_reply, sizeof(fail_reply), "OTA Update Failed: %s", esp_err_to_name(err));
            telegram_send_message(token, chat_id, fail_reply);
        }
    } 
    else {
        telegram_send_message(token, chat_id, 
            "Unknown Bot Command.\n"
            "Supported commands:\n"
            "/status - View system status\n"
            "/reboot - Reboot device\n"
            "/clear_history - Clear conversation history memory\n"
            "/clear - Clear network keys and reset\n"
            "/ota <url> - Flash firmware update over HTTPS");
    }
}

// Main Telegram Background polling task
static void telegram_bot_task(void *pvParameters)
{
    char token[64] = {0};
    if (storage_read_string("telegram_token", token, sizeof(token)) != ESP_OK || strlen(token) == 0) {
        ESP_LOGW(TAG, "No Telegram Bot Token saved in NVS. Telegram task stopped.");
        vTaskDelete(NULL);
        return;
    }

    int32_t last_update_id = 0;
    storage_read_int32("tg_last_update", &last_update_id);

    ESP_LOGI(TAG, "Telegram Bot client started. Polling updates...");

    char *recv_buf = malloc(MAX_HTTP_RECV_BUFFER);
    if (!recv_buf) {
        ESP_LOGE(TAG, "Failed to allocate memory for Telegram receiver.");
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        extern bool wifi_manager_is_connected(void);
        if (!wifi_manager_is_connected()) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        char url[256];
        snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/getUpdates?offset=%d&timeout=10", token, (int)last_update_id);

        struct HttpBuffer http_buf = {
            .data = recv_buf,
            .index = 0,
            .limit = MAX_HTTP_RECV_BUFFER
        };

        esp_http_client_config_t config = {
            .url = url,
            .method = HTTP_METHOD_GET,
            .event_handler = http_event_handler,
            .user_data = &http_buf,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .timeout_ms = 15000,
        };

        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (client) {
            esp_err_t err = esp_http_client_perform(client);
            int status_code = esp_http_client_get_status_code(client);
            
            if (err == ESP_OK && status_code == 200) {
                cJSON *json = cJSON_Parse(recv_buf);
                if (json) {
                    cJSON *ok = cJSON_GetObjectItem(json, "ok");
                    cJSON *result = cJSON_GetObjectItem(json, "result");
                    
                    if (cJSON_IsTrue(ok) && cJSON_IsArray(result)) {
                        int num_updates = cJSON_GetArraySize(result);
                        for (int i = 0; i < num_updates; i++) {
                            cJSON *update = cJSON_GetArrayItem(result, i);
                            cJSON *up_id = cJSON_GetObjectItem(update, "update_id");
                            
                            if (up_id) {
                                last_update_id = up_id->valueint + 1;
                                storage_save_int32("tg_last_update", last_update_id);
                            }

                            cJSON *message = cJSON_GetObjectItem(update, "message");
                            cJSON *chat = cJSON_GetObjectItem(message, "chat");
                            cJSON *chat_id = cJSON_GetObjectItem(chat, "id");
                            cJSON *text = cJSON_GetObjectItem(message, "text");

                            if (chat_id && text && text->valuestring) {
                                handle_telegram_message(token, chat_id->valuedouble, text->valuestring);
                            }
                        }
                    }
                    cJSON_Delete(json);
                }
            }
            esp_http_client_cleanup(client);
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    
    free(recv_buf);
}

esp_err_t telegram_init(void)
{
    BaseType_t ret = xTaskCreatePinnedToCore(telegram_bot_task, "tg_bot_task", 6144, NULL, 4, NULL, 1);
    return (ret == pdPASS) ? ESP_OK : ESP_FAIL;
}
