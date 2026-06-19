#include "backend_client.h"
#include "assistant_common.h"
#include "storage.h"
#include "oled_display.h"
#include "memory_manager.h"
#include "home_automation.h"

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
    struct HttpBuffer *buf = (struct HttpBuffer *)evt->user_data;
    
    switch(evt->event_id) {
        case HTTP_EVENT_ERROR:
            ESP_LOGE(TAG, "HTTP_EVENT_ERROR");
            break;
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGI(TAG, "TLS/TCP connection established successfully.");
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
            ESP_LOGW(TAG, "HTTP_EVENT_DISCONNECTED - connection lost or closed by server.");
            break;
        case HTTP_EVENT_REDIRECT:
            ESP_LOGD(TAG, "HTTP_EVENT_REDIRECT");
            break;
    }
    return ESP_OK;
}

esp_err_t backend_speech_to_text(const char* api_key, char* out_text, size_t max_len)
{
    ESP_LOGI(TAG, "========== [WHISPER STT] ===========");
    ESP_LOGI(TAG, "[STT] Endpoint: %s", WHISPER_API_URL);
    
    char key_buf[192] = {0};
    bool key_from_nvs = false;
    if (!api_key) {
        if (storage_read_string("openai_key", key_buf, sizeof(key_buf)) != ESP_OK) {
            ESP_LOGE(TAG, "[STT] FAILED: No 'openai_key' found in NVS storage.");
            ESP_LOGE(TAG, "[STT] Fix: Store your OpenAI key via /setkey command or captive portal.");
            return ESP_ERR_INVALID_STATE;
        }
        api_key = key_buf;
        key_from_nvs = true;
    }
    ESP_LOGI(TAG, "[STT] API key loaded: %s (first 8 chars: %.8s...)", key_from_nvs ? "from NVS" : "passed directly", api_key);

    // Open recorded WAV file from SPIFFS
    const char *filepath = "/spiffs/record.wav";
    FILE *f = fopen(filepath, "rb");
    if (!f) {
        ESP_LOGE(TAG, "[STT] FAILED: Cannot open %s — file does not exist or SPIFFS error.", filepath);
        return ESP_ERR_NOT_FOUND;
    }

    // Get file size
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    ESP_LOGI(TAG, "[STT] Audio file size: %ld bytes (%ld KB)", file_size, file_size / 1024);

    if (file_size < 100) {
        ESP_LOGE(TAG, "[STT] FAILED: Audio file too small (%ld bytes). Recording may have failed.", file_size);
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }

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
    ESP_LOGI(TAG, "[STT] Total upload payload: %ld bytes", total_post_len);

    // Allocate HTTP receiver buffer
    char *recv_buf = malloc(MAX_HTTP_RECV_BUFFER);
    if (!recv_buf) {
        fclose(f);
        ESP_LOGE(TAG, "[STT] FAILED: Out of memory allocating %d byte recv buffer.", MAX_HTTP_RECV_BUFFER);
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
        .user_data = &http_buf,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 30000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(recv_buf);
        fclose(f);
        ESP_LOGE(TAG, "[STT] FAILED: esp_http_client_init returned NULL. DNS resolution or memory failure.");
        return ESP_FAIL;
    }

    // Register for cancellation support
    assistant_register_http_client(client);

    // Setup headers
    char auth_header[512];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", api_key);
    esp_http_client_set_header(client, "Authorization", auth_header);
    
    char content_type[64];
    snprintf(content_type, sizeof(content_type), "multipart/form-data; boundary=%s", boundary);
    esp_http_client_set_header(client, "Content-Type", content_type);

    // Open connection (TLS handshake + DNS happens here)
    ESP_LOGI(TAG, "[STT] Opening TLS connection to api.openai.com...");
    esp_err_t err = esp_http_client_open(client, total_post_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[STT] FAILED: TLS/DNS connection error: %s (0x%x)", esp_err_to_name(err), err);
        ESP_LOGE(TAG, "[STT] This usually means DNS resolution failed or TLS handshake was rejected.");
        esp_http_client_cleanup(client);
        free(recv_buf);
        fclose(f);
        return err;
    }
    ESP_LOGI(TAG, "[STT] TLS connection opened successfully. Uploading audio...");

    // 1. Write multipart header
    esp_http_client_write(client, multipart_header, header_len);

    // 2. Write file data in chunks of 512 bytes
    char chunk[512];
    size_t read_bytes;
    size_t total_uploaded = 0;
    while ((read_bytes = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        if (assistant_is_cancelled()) {
            ESP_LOGW(TAG, "[STT] Cancelled during upload.");
            fclose(f);
            assistant_unregister_http_client();
            esp_http_client_cleanup(client);
            free(recv_buf);
            return ESP_ERR_INVALID_STATE;
        }
        esp_http_client_write(client, chunk, read_bytes);
        total_uploaded += read_bytes;
    }
    fclose(f);
    ESP_LOGI(TAG, "[STT] Uploaded %d bytes of audio data.", (int)total_uploaded);

    // 3. Write multipart footer
    esp_http_client_write(client, multipart_footer, footer_len);

    // Fetch response headers
    int response_len = esp_http_client_fetch_headers(client);
    if (response_len < 0) {
        ESP_LOGE(TAG, "[STT] FAILED: Connection dropped while fetching response headers.");
        esp_http_client_cleanup(client);
        free(recv_buf);
        return ESP_FAIL;
    }

    int status_code = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "[STT] HTTP Status Code: %d | Content-Length: %d", status_code, response_len);

    // Perform reading of response body via event handler
    esp_http_client_read_response(client, recv_buf, MAX_HTTP_RECV_BUFFER);

    assistant_unregister_http_client();
    esp_http_client_cleanup(client);

    if (status_code == 200) {
        // Parse JSON response
        cJSON *json = cJSON_Parse(recv_buf);
        if (json) {
            cJSON *text_item = cJSON_GetObjectItem(json, "text");
            if (text_item && text_item->valuestring) {
                strncpy(out_text, text_item->valuestring, max_len - 1);
                out_text[max_len - 1] = '\0';
                ESP_LOGI(TAG, "[STT] SUCCESS — Transcription: \"%s\"", out_text);
                cJSON_Delete(json);
                free(recv_buf);
                return ESP_OK;
            }
            cJSON_Delete(json);
        }
        ESP_LOGE(TAG, "[STT] FAILED: HTTP 200 but response JSON missing 'text' field.");
        ESP_LOGE(TAG, "[STT] Server response body: %s", recv_buf);
    } else {
        ESP_LOGE(TAG, "[STT] FAILED: Whisper API returned HTTP %d", status_code);
        ESP_LOGE(TAG, "[STT] Server response body: %s", recv_buf);
        if (status_code == 401) {
            ESP_LOGE(TAG, "[STT] 401 = Invalid API key. Check 'openai_key' in NVS.");
        } else if (status_code == 429) {
            ESP_LOGE(TAG, "[STT] 429 = Rate limited. Too many requests or quota exceeded.");
        } else if (status_code == 500 || status_code == 503) {
            ESP_LOGE(TAG, "[STT] %d = OpenAI server error. Try again later.", status_code);
        }
    }

    free(recv_buf);
    return ESP_FAIL;
}

esp_err_t backend_deepseek_chat(const char* api_key, const char* query, char* out_response, size_t max_len)
{
    ESP_LOGI(TAG, "========== [DEEPSEEK CHAT] ===========");
    ESP_LOGI(TAG, "[CHAT] Endpoint: %s", DEEPSEEK_API_URL);
    ESP_LOGI(TAG, "[CHAT] User query: \"%s\"", query);
    
    char key_buf[192] = {0};
    bool key_from_nvs = false;
    if (!api_key) {
        if (storage_read_string("deepseek_key", key_buf, sizeof(key_buf)) != ESP_OK) {
            ESP_LOGE(TAG, "[CHAT] FAILED: No 'deepseek_key' found in NVS storage.");
            ESP_LOGE(TAG, "[CHAT] Fix: Store your DeepSeek key via /setkey command or captive portal.");
            return ESP_ERR_INVALID_STATE;
        }
        api_key = key_buf;
        key_from_nvs = true;
    }
    ESP_LOGI(TAG, "[CHAT] API key loaded: %s (first 8 chars: %.8s...)", key_from_nvs ? "from NVS" : "passed directly", api_key);

    // Build JSON request payload
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", "deepseek-chat");
    cJSON *messages = cJSON_AddArrayToObject(root, "messages");
    
    // System instruction to keep responses short & concise for voice device
    cJSON *sys_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(sys_msg, "role", "system");
    
    char sys_prompt[512];
    float temp = 0, hum = 0, dist = 0;
    // Attempt to read sensors. If they fail or timeout, values remain 0, which is fine for demonstration.
    home_auto_read_dht22(&temp, &hum);
    home_auto_read_ultrasonic(&dist);
    
    snprintf(sys_prompt, sizeof(sys_prompt), 
        "You are Jarvis, a smart home voice assistant. Keep responses under 20 words. "
        "Current sensor data: Temp %.1fC, Humidity %.1f%%, Distance %.1fcm. "
        "You can control devices by including exactly ONE of these tags in your reply: [RELAY_ON], [RELAY_OFF], [SERVO_OPEN], [SERVO_CLOSE].",
        temp, hum, dist);
        
    cJSON_AddStringToObject(sys_msg, "content", sys_prompt);
    cJSON_AddItemToArray(messages, sys_msg);

    // Load and append conversation history if present
    cJSON *history_arr = memory_load_history();
    if (history_arr) {
        int hist_size = cJSON_GetArraySize(history_arr);
        ESP_LOGI(TAG, "[CHAT] Loaded %d turns from conversation history.", hist_size / 2);
        for (int i = 0; i < hist_size; i++) {
            cJSON *hist_item = cJSON_GetArrayItem(history_arr, i);
            cJSON *item_copy = cJSON_Duplicate(hist_item, true);
            cJSON_AddItemToArray(messages, item_copy);
        }
        cJSON_Delete(history_arr);
    }

    cJSON *user_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(user_msg, "role", "user");
    cJSON_AddStringToObject(user_msg, "content", query);
    cJSON_AddItemToArray(messages, user_msg);

    char *json_post_data = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    ESP_LOGI(TAG, "[CHAT] Request payload size: %d bytes", (int)strlen(json_post_data));

    // Setup HTTP Client
    char *recv_buf = malloc(MAX_HTTP_RECV_BUFFER);
    if (!recv_buf) {
        free(json_post_data);
        ESP_LOGE(TAG, "[CHAT] FAILED: Out of memory allocating recv buffer.");
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
        .user_data = &http_buf,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 30000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(recv_buf);
        free(json_post_data);
        ESP_LOGE(TAG, "[CHAT] FAILED: esp_http_client_init returned NULL. DNS or memory failure.");
        return ESP_FAIL;
    }

    // Register for cancellation support
    assistant_register_http_client(client);

    // Set headers
    char auth_header[512];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", api_key);
    esp_http_client_set_header(client, "Authorization", auth_header);
    esp_http_client_set_header(client, "Content-Type", "application/json");

    // Open connection (TLS handshake + DNS)
    ESP_LOGI(TAG, "[CHAT] Opening TLS connection to api.deepseek.com...");
    esp_err_t err = esp_http_client_open(client, strlen(json_post_data));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[CHAT] FAILED: TLS/DNS connection error: %s (0x%x)", esp_err_to_name(err), err);
        ESP_LOGE(TAG, "[CHAT] This usually means DNS resolution failed or TLS handshake was rejected.");
        esp_http_client_cleanup(client);
        free(recv_buf);
        free(json_post_data);
        return err;
    }
    ESP_LOGI(TAG, "[CHAT] TLS connection opened. Sending request...");

    esp_http_client_write(client, json_post_data, strlen(json_post_data));
    esp_http_client_fetch_headers(client);
    free(json_post_data);

    int status_code = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "[CHAT] HTTP Status Code: %d", status_code);

    esp_http_client_read_response(client, recv_buf, MAX_HTTP_RECV_BUFFER);
    assistant_unregister_http_client();
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
                    ESP_LOGI(TAG, "[CHAT] SUCCESS — Reply: \"%s\"", out_response);
                    
                    // Add this turn to conversation history
                    memory_add_to_history(query, out_response);
                    
                    cJSON_Delete(json);
                    free(recv_buf);
                    return ESP_OK;
                }
            }
            cJSON_Delete(json);
        }
        ESP_LOGE(TAG, "[CHAT] FAILED: HTTP 200 but response JSON missing expected fields.");
        ESP_LOGE(TAG, "[CHAT] Server response body: %s", recv_buf);
    } else {
        ESP_LOGE(TAG, "[CHAT] FAILED: DeepSeek API returned HTTP %d", status_code);
        ESP_LOGE(TAG, "[CHAT] Server response body: %s", recv_buf);
        if (status_code == 401) {
            ESP_LOGE(TAG, "[CHAT] 401 = Invalid API key. Check 'deepseek_key' in NVS.");
        } else if (status_code == 402) {
            ESP_LOGE(TAG, "[CHAT] 402 = Insufficient balance. Top up your DeepSeek account.");
        } else if (status_code == 429) {
            ESP_LOGE(TAG, "[CHAT] 429 = Rate limited. Too many requests.");
        } else if (status_code == 500 || status_code == 503) {
            ESP_LOGE(TAG, "[CHAT] %d = DeepSeek server error. Try again later.", status_code);
        }
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
    struct TtsFileContext *ctx = (struct TtsFileContext *)evt->user_data;
    
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
    ESP_LOGI(TAG, "========== [OPENAI TTS] ===========");
    ESP_LOGI(TAG, "[TTS] Endpoint: %s", TTS_API_URL);
    ESP_LOGI(TAG, "[TTS] Text to synthesize: \"%s\"", text);
    ESP_LOGI(TAG, "[TTS] Output file: %s", out_filepath);
    
    char key_buf[192] = {0};
    bool key_from_nvs = false;
    if (!api_key) {
        if (storage_read_string("openai_key", key_buf, sizeof(key_buf)) != ESP_OK) {
            ESP_LOGE(TAG, "[TTS] FAILED: No 'openai_key' found in NVS storage.");
            ESP_LOGE(TAG, "[TTS] Fix: Store your OpenAI key via /setkey command or captive portal.");
            return ESP_ERR_INVALID_STATE;
        }
        api_key = key_buf;
        key_from_nvs = true;
    }
    ESP_LOGI(TAG, "[TTS] API key loaded: %s (first 8 chars: %.8s...)", key_from_nvs ? "from NVS" : "passed directly", api_key);

    // Create file to save the audio stream
    FILE *f = fopen(out_filepath, "wb");
    if (!f) {
        ESP_LOGE(TAG, "[TTS] FAILED: Cannot open %s for writing. SPIFFS full or path error.", out_filepath);
        return ESP_ERR_INVALID_STATE;
    }

    struct TtsFileContext file_ctx = {
        .file = f,
        .total_written = 0
    };

    // Build OpenAI-compatible TTS JSON Request
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", "tts-1");
    cJSON_AddStringToObject(root, "input", text);
    cJSON_AddStringToObject(root, "voice", "alloy");
    cJSON_AddStringToObject(root, "response_format", "wav");

    char *json_post_data = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    ESP_LOGI(TAG, "[TTS] Request payload size: %d bytes", (int)strlen(json_post_data));

    esp_http_client_config_t config = {
        .url = TTS_API_URL,
        .method = HTTP_METHOD_POST,
        .event_handler = tts_download_event_handler,
        .user_data = &file_ctx,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 30000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        fclose(f);
        free(json_post_data);
        ESP_LOGE(TAG, "[TTS] FAILED: esp_http_client_init returned NULL. DNS or memory failure.");
        return ESP_FAIL;
    }

    // Register for cancellation support
    assistant_register_http_client(client);

    // Set headers
    char auth_header[512];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", api_key);
    esp_http_client_set_header(client, "Authorization", auth_header);
    esp_http_client_set_header(client, "Content-Type", "application/json");

    // Open connection (TLS handshake + DNS)
    ESP_LOGI(TAG, "[TTS] Opening TLS connection to api.openai.com...");
    esp_err_t err = esp_http_client_open(client, strlen(json_post_data));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[TTS] FAILED: TLS/DNS connection error: %s (0x%x)", esp_err_to_name(err), err);
        ESP_LOGE(TAG, "[TTS] This usually means DNS resolution failed or TLS handshake was rejected.");
        esp_http_client_cleanup(client);
        fclose(f);
        free(json_post_data);
        return err;
    }
    ESP_LOGI(TAG, "[TTS] TLS connection opened. Sending request...");

    esp_http_client_write(client, json_post_data, strlen(json_post_data));
    esp_http_client_fetch_headers(client);
    free(json_post_data);

    int status_code = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "[TTS] HTTP Status Code: %d", status_code);

    if (status_code != 200) {
        // For error responses, read the error body into a buffer instead of writing to file
        char err_buf[1024] = {0};
        int read_len = esp_http_client_read(client, err_buf, sizeof(err_buf) - 1);
        if (read_len > 0) err_buf[read_len] = '\0';
        fclose(f);
        esp_http_client_cleanup(client);
        unlink(out_filepath);

        ESP_LOGE(TAG, "[TTS] FAILED: OpenAI TTS API returned HTTP %d", status_code);
        ESP_LOGE(TAG, "[TTS] Server response body: %s", err_buf);
        if (status_code == 401) {
            ESP_LOGE(TAG, "[TTS] 401 = Invalid API key. Check 'openai_key' in NVS.");
        } else if (status_code == 429) {
            ESP_LOGE(TAG, "[TTS] 429 = Rate limited or quota exceeded.");
        } else if (status_code == 500 || status_code == 503) {
            ESP_LOGE(TAG, "[TTS] %d = OpenAI server error. Try again later.", status_code);
        }
        return ESP_FAIL;
    }

    // Read full stream into file
    char temp_buf[512];
    int read_len;
    while ((read_len = esp_http_client_read(client, temp_buf, sizeof(temp_buf))) > 0) {
        if (assistant_is_cancelled()) {
            ESP_LOGW(TAG, "[TTS] Cancelled during download.");
            fclose(f);
            assistant_unregister_http_client();
            esp_http_client_cleanup(client);
            unlink(out_filepath);
            return ESP_ERR_INVALID_STATE;
        }
        size_t written = fwrite(temp_buf, 1, read_len, f);
        file_ctx.total_written += written;
    }

    fclose(f);
    assistant_unregister_http_client();
    esp_http_client_cleanup(client);

    if (file_ctx.total_written > 44) {
        ESP_LOGI(TAG, "[TTS] SUCCESS — Audio downloaded: %d bytes to %s", (int)file_ctx.total_written, out_filepath);
        return ESP_OK;
    }

    ESP_LOGE(TAG, "[TTS] FAILED: Downloaded file too small (%d bytes). Expected WAV with >44 byte header.", (int)file_ctx.total_written);
    unlink(out_filepath);
    return ESP_FAIL;
}
