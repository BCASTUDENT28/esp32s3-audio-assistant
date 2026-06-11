#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <esp_err.h>
#include <stdbool.h>

/**
 * @brief Initialize and start the WiFi subsystem.
 * Tries to connect to stored credentials from NVS. If none are stored or connection fails
 * after several retries, it switches to AP Mode (SSID: "ESP32-S3-Assistant") and launches 
 * a captive setup portal web server.
 * 
 * @param wait_for_connect Block until WiFi is connected or falls back to AP.
 * @return esp_err_t 
 */
esp_err_t wifi_manager_init(bool wait_for_connect);

/**
 * @brief Get the connection status of WiFi.
 * @return true if connected with an IP address in station mode, false otherwise.
 */
bool wifi_manager_is_connected(void);

#endif // WIFI_MANAGER_H
