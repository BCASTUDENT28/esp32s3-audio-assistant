#ifndef STORAGE_H
#define STORAGE_H

#include <esp_err.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define STORAGE_MOUNT_POINT "/spiffs"

/**
 * @brief Initialize secure NVS and mount SPIFFS filesystem.
 * @return ESP_OK on success, appropriate error code on failure.
 */
esp_err_t storage_init(void);

/**
 * @brief Save a string value to NVS.
 * 
 * @param key Config key.
 * @param value String value to save.
 * @return esp_err_t 
 */
esp_err_t storage_save_string(const char* key, const char* value);

/**
 * @brief Read a string value from NVS.
 * 
 * @param key Config key.
 * @param buffer Output buffer.
 * @param max_len Size of output buffer.
 * @return esp_err_t 
 */
esp_err_t storage_read_string(const char* key, char* buffer, size_t max_len);

/**
 * @brief Save an int32 value to NVS.
 */
esp_err_t storage_save_int32(const char* key, int32_t value);

/**
 * @brief Read an int32 value from NVS.
 */
esp_err_t storage_read_int32(const char* key, int32_t* value);

/**
 * @brief Check if WiFi credentials are saved.
 */
bool storage_is_wifi_configured(void);

/**
 * @brief Clear all stored NVS configurations (resets settings).
 */
void storage_clear_all(void);

#endif // STORAGE_H
