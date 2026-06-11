#ifndef HOME_AUTOMATION_H
#define HOME_AUTOMATION_H

#include <esp_err.h>
#include <stdbool.h>

/**
 * @brief Initialize all home automation peripherals (Relay, Servo, DHT22, Ultrasonic).
 * 
 * @return esp_err_t 
 */
esp_err_t home_auto_init(void);

/**
 * @brief Set Relay state.
 * 
 * @param active true to turn relay ON, false to turn OFF.
 * @return esp_err_t 
 */
esp_err_t home_auto_set_relay(bool active);

/**
 * @brief Set Servo angle.
 * 
 * @param angle Angle in degrees (0 to 180).
 * @return esp_err_t 
 */
esp_err_t home_auto_set_servo_angle(int angle);

/**
 * @brief Read data from DHT22 sensor.
 * 
 * @param out_temp Pointer to store temperature in Celsius.
 * @param out_humidity Pointer to store relative humidity percentage.
 * @return esp_err_t ESP_OK on success, ESP_ERR_TIMEOUT or ESP_FAIL on failure.
 */
esp_err_t home_auto_read_dht22(float *out_temp, float *out_humidity);

/**
 * @brief Measure distance using HC-SR04 Ultrasonic sensor.
 * 
 * @param out_distance_cm Pointer to store distance in centimeters.
 * @return esp_err_t ESP_OK on success, ESP_ERR_TIMEOUT on failure.
 */
esp_err_t home_auto_read_ultrasonic(float *out_distance_cm);

#endif // HOME_AUTOMATION_H
