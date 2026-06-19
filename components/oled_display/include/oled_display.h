#ifndef OLED_DISPLAY_H
#define OLED_DISPLAY_H

#include <esp_err.h>

typedef enum {
    OLED_STATE_BOOT,
    OLED_STATE_WIFI_PORTAL,
    OLED_STATE_WIFI_CONNECTING,
    OLED_STATE_READY,
    OLED_STATE_LISTENING,
    OLED_STATE_TRANSCRIBING,
    OLED_STATE_THINKING,
    OLED_STATE_GENERATING_VOICE,
    OLED_STATE_SPEAKING,
    OLED_STATE_OTA,
    OLED_STATE_ERROR
} oled_state_t;

/**
 * @brief Initialize the I2C master driver and the SSD1306 OLED display.
 * @return esp_err_t 
 */
esp_err_t oled_init(void);

/**
 * @brief Set the current status/state of the display.
 * This triggers corresponding text and animations.
 * 
 * @param state State enum.
 */
void oled_set_state(oled_state_t state);

/**
 * @brief Display a custom title and message on the OLED.
 * 
 * @param title Bold header text.
 * @param msg Body message (will wrap or truncate).
 */
void oled_set_custom_message(const char* title, const char* msg);

/**
 * @brief Update the microphone level visualization.
 * Used during recording to show dynamic audio wave response.
 * 
 * @param level Audio level between 0.0 (silent) and 1.0 (clipping).
 */
void oled_set_mic_level(float level);

#endif // OLED_DISPLAY_H
