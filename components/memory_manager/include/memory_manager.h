#ifndef MEMORY_MANAGER_H
#define MEMORY_MANAGER_H

#include <esp_err.h>
#include <cJSON.h>

/**
 * @brief Initialize the memory manager.
 */
esp_err_t memory_manager_init(void);

/**
 * @brief Add a query-response pair to the conversation history on SPIFFS.
 * 
 * @param query The user's query text.
 * @param response The assistant's response text.
 * @return esp_err_t 
 */
esp_err_t memory_add_to_history(const char* query, const char* response);

/**
 * @brief Load the conversation history as a cJSON array of messages.
 * Caller is responsible for deleting the returned cJSON array object.
 * 
 * @return cJSON* Pointer to the cJSON array, or NULL if empty/error.
 */
cJSON* memory_load_history(void);

/**
 * @brief Clear the conversation history stored on SPIFFS.
 */
void memory_clear_history(void);

/**
 * @brief Save user preference or profile value (stored in NVS).
 */
esp_err_t memory_save_preference(const char* key, const char* value);

/**
 * @brief Retrieve user preference or profile value (from NVS).
 */
esp_err_t memory_get_preference(const char* key, char* out_val, size_t max_len);

#endif // MEMORY_MANAGER_H
