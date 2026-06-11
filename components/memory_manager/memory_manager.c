#include "memory_manager.h"
#include "storage.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <esp_log.h>

static const char *TAG = "MEM_MGR";
#define HISTORY_FILE "/spiffs/history.json"
#define MAX_HISTORY_TURNS 4 // Keep last 4 turns (8 messages)

esp_err_t memory_manager_init(void)
{
    ESP_LOGI(TAG, "Memory Manager Initialized.");
    return ESP_OK;
}

esp_err_t memory_add_to_history(const char* query, const char* response)
{
    cJSON *history_arr = NULL;
    
    // Read existing history file if it exists
    FILE *f = fopen(HISTORY_FILE, "r");
    if (f) {
        fseek(f, 0, SEEK_END);
        long file_size = ftell(f);
        fseek(f, 0, SEEK_SET);
        
        if (file_size > 0) {
            char *buf = malloc(file_size + 1);
            if (buf) {
                size_t read_bytes = fread(buf, 1, file_size, f);
                buf[read_bytes] = '\0';
                history_arr = cJSON_Parse(buf);
                free(buf);
            }
        }
        fclose(f);
    }
    
    if (!history_arr || !cJSON_IsArray(history_arr)) {
        if (history_arr) cJSON_Delete(history_arr);
        history_arr = cJSON_CreateArray();
    }
    
    // Add user query message
    cJSON *user_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(user_msg, "role", "user");
    cJSON_AddStringToObject(user_msg, "content", query);
    cJSON_AddItemToArray(history_arr, user_msg);
    
    // Add assistant response message
    cJSON *assistant_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(assistant_msg, "role", "assistant");
    cJSON_AddStringToObject(assistant_msg, "content", response);
    cJSON_AddItemToArray(history_arr, assistant_msg);
    
    // Prune history if it exceeds limit (2 items per turn: user and assistant)
    int max_messages = MAX_HISTORY_TURNS * 2;
    while (cJSON_GetArraySize(history_arr) > max_messages) {
        cJSON_DeleteItemFromArray(history_arr, 0); // Remove oldest user message
        cJSON_DeleteItemFromArray(history_arr, 0); // Remove oldest assistant response
    }
    
    // Write back to file
    char *json_str = cJSON_PrintUnformatted(history_arr);
    cJSON_Delete(history_arr);
    
    if (!json_str) {
        ESP_LOGE(TAG, "Failed to print history JSON");
        return ESP_FAIL;
    }
    
    f = fopen(HISTORY_FILE, "w");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open history file for writing");
        free(json_str);
        return ESP_FAIL;
    }
    
    fprintf(f, "%s", json_str);
    fclose(f);
    free(json_str);
    
    ESP_LOGI(TAG, "History saved to SPIFFS.");
    return ESP_OK;
}

cJSON* memory_load_history(void)
{
    FILE *f = fopen(HISTORY_FILE, "r");
    if (!f) {
        ESP_LOGD(TAG, "History file not found, starting fresh.");
        return NULL;
    }
    
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    if (file_size <= 0) {
        fclose(f);
        return NULL;
    }
    
    char *buf = malloc(file_size + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    
    size_t read_bytes = fread(buf, 1, file_size, f);
    buf[read_bytes] = '\0';
    fclose(f);
    
    cJSON *history_arr = cJSON_Parse(buf);
    free(buf);
    
    if (!history_arr || !cJSON_IsArray(history_arr)) {
        if (history_arr) cJSON_Delete(history_arr);
        return NULL;
    }
    
    return history_arr;
}

void memory_clear_history(void)
{
    unlink(HISTORY_FILE);
    ESP_LOGI(TAG, "History file deleted.");
}

esp_err_t memory_save_preference(const char* key, const char* value)
{
    return storage_save_string(key, value);
}

esp_err_t memory_get_preference(const char* key, char* out_val, size_t max_len)
{
    return storage_read_string(key, out_val, max_len);
}
