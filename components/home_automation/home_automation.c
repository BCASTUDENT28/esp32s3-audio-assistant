#include "home_automation.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/gpio.h>
#include <driver/ledc.h>
#include <esp_timer.h>
#include <esp_log.h>
#include <esp_rom_sys.h>

static const char *TAG = "HOME_AUTO";

#define RELAY_GPIO        GPIO_NUM_10
#define SERVO_GPIO        GPIO_NUM_11
#define DHT22_GPIO        GPIO_NUM_12
#define ULTRASONIC_TRIG   GPIO_NUM_13
#define ULTRASONIC_ECHO   GPIO_NUM_14

#define SERVO_LEDC_TIMER  LEDC_TIMER_0
#define SERVO_LEDC_CHAN   LEDC_CHANNEL_0

static portMUX_TYPE dht_mux = portMUX_INITIALIZER_UNLOCKED;

esp_err_t home_auto_init(void)
{
    ESP_LOGI(TAG, "Initializing Home Automation peripherals...");

    // 1. Configure Relay Pin (Output)
    gpio_config_t relay_cfg = {
        .pin_bit_mask = (1ULL << RELAY_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&relay_cfg);
    gpio_set_level(RELAY_GPIO, 0); // Default relay OFF
    ESP_LOGI(TAG, "Relay initialized on GPIO %d", RELAY_GPIO);

    // 2. Configure Servo Pin via LEDC (50Hz PWM, 14-bit resolution)
    ledc_timer_config_t ledc_timer = {
        .speed_mode       = LEDC_LOW_SPEED_MODE,
        .timer_num        = SERVO_LEDC_TIMER,
        .duty_resolution  = LEDC_TIMER_14_BIT,
        .freq_hz          = 50, // 50Hz for standard RC servos
        .clk_cfg          = LEDC_AUTO_CLK
    };
    esp_err_t err = ledc_timer_config(&ledc_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Servo LEDC timer config failed: %s", esp_err_to_name(err));
        return err;
    }

    ledc_channel_config_t ledc_channel = {
        .speed_mode     = LEDC_LOW_SPEED_MODE,
        .channel        = SERVO_LEDC_CHAN,
        .timer_sel      = SERVO_LEDC_TIMER,
        .intr_type      = LEDC_INTR_DISABLE,
        .gpio_num       = SERVO_GPIO,
        .duty           = 0,
        .hpoint         = 0
    };
    err = ledc_channel_config(&ledc_channel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Servo LEDC channel config failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "Servo initialized on GPIO %d", SERVO_GPIO);

    // 3. Configure DHT22 Pin (Input with pullup)
    gpio_config_t dht_cfg = {
        .pin_bit_mask = (1ULL << DHT22_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&dht_cfg);
    ESP_LOGI(TAG, "DHT22 initialized on GPIO %d", DHT22_GPIO);

    // 4. Configure Ultrasonic Pins (Trigger = Output, Echo = Input)
    gpio_config_t trig_cfg = {
        .pin_bit_mask = (1ULL << ULTRASONIC_TRIG),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&trig_cfg);
    gpio_set_level(ULTRASONIC_TRIG, 0);

    gpio_config_t echo_cfg = {
        .pin_bit_mask = (1ULL << ULTRASONIC_ECHO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&echo_cfg);
    ESP_LOGI(TAG, "Ultrasonic HC-SR04 initialized (Trig:%d, Echo:%d)", ULTRASONIC_TRIG, ULTRASONIC_ECHO);

    return ESP_OK;
}

esp_err_t home_auto_set_relay(bool active)
{
    ESP_LOGI(TAG, "Setting Relay to %s", active ? "ON" : "OFF");
    return gpio_set_level(RELAY_GPIO, active ? 1 : 0);
}

esp_err_t home_auto_set_servo_angle(int angle)
{
    if (angle < 0) angle = 0;
    if (angle > 180) angle = 180;

    // Standard servo: 0.5ms to 2.5ms pulse over 20ms period (50Hz).
    // Resolution = 14-bit (16384 total steps).
    // 0.5ms = 16384 * (0.5 / 20) = 410 steps
    // 2.5ms = 16384 * (2.5 / 20) = 2048 steps
    uint32_t duty = 410 + (angle * (2048 - 410) / 180);
    
    ESP_LOGI(TAG, "Setting Servo angle to %d degrees (LEDC duty: %lu)", angle, (unsigned long)duty);
    esp_err_t err = ledc_set_duty(LEDC_LOW_SPEED_MODE, SERVO_LEDC_CHAN, duty);
    if (err == ESP_OK) {
        err = ledc_update_duty(LEDC_LOW_SPEED_MODE, SERVO_LEDC_CHAN);
    }
    return err;
}

static int wait_or_timeout(gpio_num_t pin, int level, uint32_t timeout_us)
{
    uint64_t start = esp_timer_get_time();
    while (gpio_get_level(pin) == level) {
        if ((esp_timer_get_time() - start) > timeout_us) {
            return -1; // Timeout
        }
    }
    return (int)(esp_timer_get_time() - start);
}

esp_err_t home_auto_read_dht22(float *out_temp, float *out_humidity)
{
    uint8_t data[5] = {0};
    
    // 1. Send start signal: pull low for 20ms, then let high for 30us
    gpio_set_direction(DHT22_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(DHT22_GPIO, 0);
    esp_rom_delay_us(20000);
    gpio_set_level(DHT22_GPIO, 1);
    esp_rom_delay_us(30);
    
    // 2. Switch pin to input
    gpio_set_direction(DHT22_GPIO, GPIO_MODE_INPUT);
    
    taskENTER_CRITICAL(&dht_mux);
    
    // DHT22 pulls pin low for 80us, then high for 80us
    if (wait_or_timeout(DHT22_GPIO, 0, 100) < 0) {
        taskEXIT_CRITICAL(&dht_mux);
        return ESP_ERR_TIMEOUT;
    }
    if (wait_or_timeout(DHT22_GPIO, 1, 100) < 0) {
        taskEXIT_CRITICAL(&dht_mux);
        return ESP_ERR_TIMEOUT;
    }
    
    // Read 40 data bits
    for (int i = 0; i < 40; i++) {
        // Wait for low phase (50us) to end
        if (wait_or_timeout(DHT22_GPIO, 0, 80) < 0) {
            taskEXIT_CRITICAL(&dht_mux);
            return ESP_FAIL;
        }
        
        // Measure duration of high phase (26-28us for '0', 70us for '1')
        int duration = wait_or_timeout(DHT22_GPIO, 1, 100);
        if (duration < 0) {
            taskEXIT_CRITICAL(&dht_mux);
            return ESP_FAIL;
        }
        
        // If high phase is longer than 40us, bit is '1'
        if (duration > 40) {
            data[i / 8] |= (1 << (7 - (i % 8)));
        }
    }
    
    taskEXIT_CRITICAL(&dht_mux);
    
    // 3. Verify Checksum
    uint8_t checksum = (data[0] + data[1] + data[2] + data[3]) & 0xFF;
    if (checksum != data[4]) {
        ESP_LOGE(TAG, "DHT22 checksum mismatch: calculated %02x, read %02x", checksum, data[4]);
        return ESP_ERR_INVALID_CRC;
    }
    
    // 4. Decode temperature and humidity
    int16_t raw_humidity = (data[0] << 8) | data[1];
    int16_t raw_temp = ((data[2] & 0x7F) << 8) | data[3];
    if (data[2] & 0x80) {
        raw_temp = -raw_temp; // Negative temperature
    }
    
    *out_humidity = (float)raw_humidity / 10.0f;
    *out_temp = (float)raw_temp / 10.0f;
    
    ESP_LOGI(TAG, "DHT22 raw read success: Temp=%.1f C, Humidity=%.1f %%", *out_temp, *out_humidity);
    return ESP_OK;
}

esp_err_t home_auto_read_ultrasonic(float *out_distance_cm)
{
    // Trigger pulse: 10us High pulse
    gpio_set_level(ULTRASONIC_TRIG, 0);
    esp_rom_delay_us(2);
    gpio_set_level(ULTRASONIC_TRIG, 1);
    esp_rom_delay_us(10);
    gpio_set_level(ULTRASONIC_TRIG, 0);

    // Wait for Echo pin to go High (timeout 20ms)
    uint64_t start_time = esp_timer_get_time();
    while (gpio_get_level(ULTRASONIC_ECHO) == 0) {
        if ((esp_timer_get_time() - start_time) > 20000) {
            ESP_LOGW(TAG, "Ultrasonic echo start timeout");
            return ESP_ERR_TIMEOUT;
        }
    }

    uint64_t echo_start = esp_timer_get_time();
    
    // Wait for Echo pin to go Low (timeout 30ms)
    while (gpio_get_level(ULTRASONIC_ECHO) == 1) {
        if ((esp_timer_get_time() - echo_start) > 30000) {
            ESP_LOGW(TAG, "Ultrasonic echo end timeout");
            return ESP_ERR_TIMEOUT;
        }
    }

    uint64_t echo_end = esp_timer_get_time();
    uint64_t duration_us = echo_end - echo_start;

    // Sound travels at 343 m/s = 0.0343 cm/us.
    // Distance = (time * speed) / 2 (one-way trip)
    *out_distance_cm = (float)duration_us * 0.01715f;

    ESP_LOGI(TAG, "Ultrasonic distance read: %.2f cm", *out_distance_cm);
    return ESP_OK;
}
