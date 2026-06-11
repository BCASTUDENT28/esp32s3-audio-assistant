#ifndef OTA_H
#define OTA_H

#include <esp_err.h>

/**
 * @brief Trigger an HTTPS OTA firmware update in a background task.
 * Updates the OLED status display to OTA, downloads the new firmware binary,
 * flashes it to the passive partition, and reboots the device upon success.
 * 
 * @param url The HTTPS URL to download the binary from.
 * @return esp_err_t 
 */
esp_err_t ota_start(const char *url);

#endif // OTA_H
