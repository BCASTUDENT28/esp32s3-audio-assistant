#ifndef TELEGRAM_H
#define TELEGRAM_H

#include <esp_err.h>

/**
 * @brief Initialize and start the Telegram Bot control interface.
 * Retrieves the Telegram token from secure NVS and spawns a background polling task 
 * to handle control commands (/status, /reboot, /ota, /clear).
 * 
 * @return esp_err_t 
 */
esp_err_t telegram_init(void);

#endif // TELEGRAM_H
