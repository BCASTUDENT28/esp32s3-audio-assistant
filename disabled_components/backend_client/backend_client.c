#include "backend_client.h"
#include "storage.h"
#include "oled_display.h"

#include <string.h>
#include <esp_log.h>
#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#include <cJSON.h>

static const char *TAG = "BACKEND";

// Default API Endpoints
#define WHISPER_API_URL  "https://api.openai.com/v1/audio/transcriptions"
#define DEEPSEEK_API_URL "https://api.deepseek.com/v1/chat/completions"
#define TTS_API_URL      "https://api.openai.com/v1/audio/speech"

// HTTP response buffer config
#define MAX_HTTP_RECV_BUFFER 4096

// Structure to receive data during HTTP GET/POST
struct HttpBuffer {
    char *data;
    int index;
    int limit;
};

// HTTP Event Handler
static esp_err_t http_event_handler(esp_http_client_event_handle_t evt)
{
    struct HttpBuffer *buf = (struct HttpBuffer *)evt->user_ctx;
    
    switch(evt->event_id) {
        case HTTP_EVENT_ERROR:
            ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
            break;
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
            break;
        case HTTP_EVENT_HEADER_SENT:
            ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
            break;
        case HTTP_EVENT_ON_HEADER:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER: %s = %s", evt->header_key, evt->header_value);
            break;
        case HTTP_EVENT_ON_DATA:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
            if (buf && buf->data && (buf->index + evt->data_len < buf->limit)) {
                memcpy(buf->data + buf->index, evt->data, evt->data_len);
                buf->index += evt->data_len;
                buf->data[buf->index] = '\0';
            }
            break;
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
            break;
        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGD(TAG, "HTTP_EVENT_DISCONNECTED");
            break;
        case HTTP_EVENT_REDIRECT:
            ESP_LOGD(TAG, "HTTP_EVENT_REDIRECT");
            break;
    }
    return ESP_OK;
}

esp_err_t backend_speech_to_text(const char* api_key, char* out_text, size_t max_len)
{
    ESP_LOGI(TAG, "Transcribing speech to text via Whisper API...");
    
    char key_buf[96] = {0};
    if (!api_key) {
        if (storage_read_string("deepseek_key", key_buf, sizeof(key_buf)) != ESP_OK) {
            ESP_LOGE(TAG, "No API key available for STT.");
            return ESP_ERR_INVALID_STATE;
        }
        api_key = key_buf;
    }

    // Open recorded WAV file from SPIFFS
    const char *filepath = "/spiffs/record.wav";
    FILE *f = fopen(filepath, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open record.wav for upload");
        return ESP_ERR_NOT_FOUND;
    }

    // Get file size
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    // Prepare multipart boundaries and headers
    const char *boundary = "ESP32Boundary";
    char multipart_header[256];
    int header_len = snprintf(multipart_header, sizeof(multipart_header),
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"record.wav\"\r\n"
        "Content-Type: audio/wav\r\n\r\n", boundary);

    char multipart_footer[256];
    int footer_len = snprintf(multipart_footer, sizeof(multipart_footer),
        "\r\n--%s\r\n"
        "Content-Disposition: form-data; name=\"model\"\r\n\r\n"
        "whisper-1\r\n"
        "--%s--\r\n", boundary, boundary);

    long total_post_len = header_len + file_size + footer_len;

    // Allocate HTTP receiver buffer
    char *recv_buf = malloc(MAX_HTTP_RECV_BUFFER);
    if (!recv_buf) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    recv_buf[0] = '\0';

    struct HttpBuffer http_buf = {
        .data = recv_buf,
        .index = 0,
        .limit = MAX_HTTP_RECV_BUFFER
    };

    esp_http_client_config_t config = {
        .url = WHISPER_API_URL,
        .method = HTTP_METHOD_POST,
        .event_handler = http_event_handler,
        .user_ctx = &http_buf,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(recv_buf);
        fclose(f);
        return ESP_FAIL;
    }

    // Setup headers
    char auth_header[128];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", api_key);
    esp_http_client_set_header(client, "Authorization", auth_header);
    
    char content_type[64];
    snprintf(content_type, sizeof(content_type), "multipart/form-data; boundary=%s", boundary);
    esp_http_client_set_header(client, "Content-Type", content_type);

    // Open connection and write payload in chunks (prevents RAM exhaustion)
    esp_err_t err = esp_http_client_open(client, total_post_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTPS client: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        free(recv_buf);
        fclose(f);
        return err;
    }

    // 1. Write multipart header
    esp_http_client_write(client, multipart_header, header_len);

    // 2. Write file data in chunks of 512 bytes
    char chunk[512];
    size_t read_bytes;
    while ((read_bytes = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        esp_http_client_write(client, chunk, read_bytes);
    }
    fclose(f);

    // 3. Write multipart footer
    esp_http_client_write(client, multipart_footer, footer_len);

    // Fetch response headers
    int response_len = esp_http_client_fetch_headers(client);
    if (response_len < 0) {
        ESP_LOGE(TAG, "HTTP connection error fetching headers");
        esp_http_client_cleanup(client);
        free(recv_buf);
        return ESP_FAIL;
    }

    int status_code = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "Whisper API Status Code: %d, Response Len: %d", status_code, response_len);

    // Perform reading of response body via event handler
    esp_http_client_read_response(client, recv_buf, MAX_HTTP_RECV_BUFFER);

    esp_http_client_cleanup(client);

    if (status_code == 200) {
        // Parse JSON response
        cJSON *json = cJSON_Parse(recv_buf);
        if (json) {
            cJSON *text_item = cJSON_GetObjectItem(json, "text");
            if (text_item && text_item->valuestring) {
                strncpy(out_text, text_item->valuestring, max_len - 1);
                out_text[max_len - 1] = '\0';
                ESP_LOGI(TAG, "Transcribed: %s", out_text);
                cJSON_Delete(json);
                free(recv_buf);
                return ESP_OK;
            }
            cJSON_Delete(json);
        }
    } else {
        ESP_LOGE(TAG, "Whisper STT Server Error: %s", recv_buf);
    }

    free(recv_buf);
    return ESP_FAIL;
}

esp_err_t backend_deepseek_chat(const char* api_key, const char* query, char* out_response, size_t max_len)
{
    ESP_LOGI(TAG, "Sending query to DeepSeek: %s", query);
    
    char key_buf[96] = {0};
    if (!api_key) {
        if (storage_read_string("deepseek_key", key_buf, sizeof(key_buf)) != ESP_OK) {
            ESP_LOGE(TAG, "No API key available for DeepSeek.");
            return ESP_ERR_INVALID_STATE;
        }
        api_key = key_buf;
    }

    // Build JSON request payload
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", "deepseek-chat");
    cJSON *messages = cJSON_AddArrayToObject(root, "messages");
    
    // System instruction to keep responses short & concise for voice device
    cJSON *sys_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(sys_msg, "role", "system");
    cJSON_AddStringToObject(sys_msg, "content", "You are a helpful voice assistant. Keep responses under 20 words.");
    cJSON_AddItemToArray(messages, sys_msg);

    cJSON *user_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(user_msg, "role", "user");
    cJSON_AddStringToObject(user_msg, "content", query);
    cJSON_AddItemToArray(messages, user_msg);

    char *json_post_data = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    // Setup HTTP Client
    char *recv_buf = malloc(MAX_HTTP_RECV_BUFFER);
    if (!recv_buf) {
        free(json_post_data);
        return ESP_ERR_NO_MEM;
    }
    recv_buf[0] = '\0';

    struct HttpBuffer http_buf = {
        .data = recv_buf,
        .index = 0,
        .limit = MAX_HTTP_RECV_BUFFER
    };

    esp_http_client_config_t config = {
        .url = DEEPSEEK_API_URL,
        .method = HTTP_METHOD_POST,
        .event_handler = http_event_handler,
        .user_ctx = &http_buf,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(recv_buf);
        free(json_post_data);
        return ESP_FAIL;
    }

    // Set headers
    char auth_header[128];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", api_key);
    esp_http_client_set_header(client, "Authorization", auth_header);
    esp_http_client_set_header(client, "Content-Type", "application/json");

    // Write POST body
    esp_err_t err = esp_http_client_open(client, strlen(json_post_data));
    if (err == ESP_OK) {
        esp_http_client_write(client, json_post_data, strlen(json_post_data));
        esp_http_client_fetch_headers(client);
    }
    free(json_post_data);

    int status_code = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "DeepSeek API Status Code: %d", status_code);

    esp_http_client_read_response(client, recv_buf, MAX_HTTP_RECV_BUFFER);
    esp_http_client_cleanup(client);

    if (status_code == 200) {
        cJSON *json = cJSON_Parse(recv_buf);
        if (json) {
            cJSON *choices = cJSON_GetObjectItem(json, "choices");
            if (cJSON_IsArray(choices) && cJSON_GetArraySize(choices) > 0) {
                cJSON *choice = cJSON_GetArrayItem(choices, 0);
                cJSON *message = cJSON_GetObjectItem(choice, "message");
                cJSON *content = cJSON_GetObjectItem(message, "content");
                if (content && content->valuestring) {
                    strncpy(out_response, content->valuestring, max_len - 1);
                    out_response[max_len - 1] = '\0';
                    ESP_LOGI(TAG, "DeepSeek Reply: %s", out_response);
                    cJSON_Delete(json);
                    free(recv_buf);
                    return ESP_OK;
                }
            }
            cJSON_Delete(json);
        }
    } else {
        ESP_LOGE(TAG, "DeepSeek API Error response: %s", recv_buf);
    }

    free(recv_buf);
    return ESP_FAIL;
}

// Struct to track TTS file download write status
struct TtsFileContext {
    FILE *file;
    size_t total_written;
};

// TTS HTTP Event Handler to stream audio directly to SPIFFS without accumulating in RAM
static esp_err_t tts_download_event_handler(esp_http_client_event_handle_t evt)
{
    struct TtsFileContext *ctx = (struct TtsFileContext *)evt->user_ctx;
    
    switch(evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (ctx && ctx->file && evt->data_len > 0) {
                size_t written = fwrite(evt->data, 1, evt->data_len, ctx->file);
                ctx->total_written += written;
                ESP_LOGD(TAG, "Downloaded %d bytes to file", evt->data_len);
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

esp_err_t backend_text_to_speech(const char* api_key, const char* text, const char* out_filepath)
{
    ESP_LOGI(TAG, "Synthesizing TTS for: %s", text);
    
    char key_buf[96] = {0};
    if (!api_key) {
        if (storage_read_string("deepseek_key", key_buf, sizeof(key_buf)) != ESP_OK) {
            ESP_LOGE(TAG, "No API key available for TTS.");
            return ESP_ERR_INVALID_STATE;
        }
        api_key = key_buf;
    }

    // Create file to save the audio stream
    FILE *f = fopen(out_filepath, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open target path %s for TTS write", out_filepath);
        return ESP_ERR_INVALID_STATE;
    }

    struct TtsFileContext file_ctx = {
        .file = f,
        .total_written = 0
    };

    // Build OpenAI-compatible TTS JSON Request
    // We request 'wav' format or high quality 'pcm' so that we can easily play it on our 16-bit DAC!
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", "tts-1");
    cJSON_AddStringToObject(root, "input", text);
    cJSON_AddStringToObject(root, "voice", "alloy");
    cJSON_AddStringToObject(root, "response_format", "wav"); // Request standard WAV (which has 44-byte header then raw 16-bit 24kHz/16kHz PCM)

    char *json_post_data = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    esp_http_client_config_t config = {
        .url = TTS_API_URL,
        .method = HTTP_METHOD_POST,
        .event_handler = tts_download_event_handler,
        .user_ctx = &file_ctx,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        fclose(f);
        free(json_post_data);
        return ESP_FAIL;
    }

    // Set headers
    char auth_header[128];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", api_key);
    esp_http_client_set_header(client, "Authorization", auth_header);
    esp_http_client_set_header(client, "Content-Type", "application/json");

    // Write POST body
    esp_err_t err = esp_http_client_open(client, strlen(json_post_data));
    if (err == ESP_OK) {
        esp_http_client_write(client, json_post_data, strlen(json_post_data));
        esp_http_client_fetch_headers(client);
    }
    free(json_post_data);

    int status_code = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "TTS API Status Code: %d", status_code);

    // Read full stream into file (handled inside our download event handler)
    char temp_buf[512];
    int read_len;
    while ((read_len = esp_http_client_read(client, temp_buf, sizeof(temp_buf))) > 0) {
        // Bytes are written to file inside event handler
    }

    fclose(f);
    esp_http_client_cleanup(client);

    if (status_code == 200 && file_ctx.total_written > 44) {
        ESP_LOGI(TAG, "TTS Audio file downloaded successfully, size: %d bytes", file_ctx.total_written);
        return ESP_OK;
    }

    ESP_LOGE(TAG, "Failed to download TTS audio stream (status: %d)", status_code);
    unlink(out_filepath); // Remove invalid empty file
    return ESP_FAIL;
}
