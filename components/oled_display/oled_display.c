#include "oled_display.h"
#include "font.h"
#include <string.h>
#include <math.h>
#include <esp_log.h>
#include <driver/i2c_master.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

static const char *TAG = "OLED";

#define I2C_PORT            I2C_NUM_1
#define SDA_GPIO            9
#define SCL_GPIO            8
#define SSD1306_I2C_ADDR    0x3C

// Screen dimensions
#define SCREEN_WIDTH        128
#define SCREEN_HEIGHT       64
#define FB_SIZE             (SCREEN_WIDTH * SCREEN_HEIGHT / 8)

// Global state
static i2c_master_dev_handle_t oled_dev_handle = NULL;
static uint8_t frame_buffer[FB_SIZE];
static SemaphoreHandle_t fb_mutex = NULL;
static oled_state_t current_state = OLED_STATE_BOOT;
static float current_mic_level = 0.0f;
static char custom_title[24] = "";
static char custom_message[64] = "";
static bool is_custom_message_active = false;
static TaskHandle_t oled_task_handle = NULL;

// SSD1306 Write Helper Functions
static esp_err_t ssd1306_write_cmd(uint8_t cmd)
{
    if (!oled_dev_handle) return ESP_ERR_INVALID_STATE;
    uint8_t write_buf[2] = {0x00, cmd}; // 0x00 for Command
    return i2c_master_transmit(oled_dev_handle, write_buf, 2, 1000);
}

static esp_err_t ssd1306_write_data(const uint8_t *data, size_t len)
{
    if (!oled_dev_handle) return ESP_ERR_INVALID_STATE;
    uint8_t *write_buf = malloc(len + 1);
    if (!write_buf) return ESP_ERR_NO_MEM;
    write_buf[0] = 0x40; // 0x40 for Data
    memcpy(&write_buf[1], data, len);
    esp_err_t err = i2c_master_transmit(oled_dev_handle, write_buf, len + 1, 1000);
    free(write_buf);
    return err;
}

// Drawing Utilities
static void oled_clear_buffer(void)
{
    memset(frame_buffer, 0, sizeof(frame_buffer));
}

static void oled_draw_pixel(int x, int y, bool color)
{
    if (x < 0 || x >= SCREEN_WIDTH || y < 0 || y >= SCREEN_HEIGHT) return;
    if (color) {
        frame_buffer[x + (y / 8) * SCREEN_WIDTH] |= (1 << (y % 8));
    } else {
        frame_buffer[x + (y / 8) * SCREEN_WIDTH] &= ~(1 << (y % 8));
    }
}

static void oled_draw_char(int x, int y, char c)
{
    if (c < 32 || c > 126) c = ' ';
    int font_idx = c - 32;
    for (int col = 0; col < 5; col++) {
        uint8_t line = font_5x7[font_idx][col];
        for (int bit = 0; bit < 8; bit++) {
            if (line & (1 << bit)) {
                oled_draw_pixel(x + col, y + bit, true);
            }
        }
    }
}

static void oled_draw_string(int x, int y, const char *str)
{
    while (*str) {
        oled_draw_char(x, y, *str++);
        x += 6; // 5 pixels font width + 1 pixel space
        if (x + 5 >= SCREEN_WIDTH) break; // Simple clipping
    }
}

static void oled_draw_line(int x0, int y0, int x1, int y1, bool color)
{
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy, e2;

    while (1) {
        oled_draw_pixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void oled_draw_circle(int xc, int yc, int r, bool color)
{
    int x = 0, y = r;
    int d = 3 - 2 * r;
    while (y >= x) {
        oled_draw_pixel(xc + x, yc + y, color);
        oled_draw_pixel(xc - x, yc + y, color);
        oled_draw_pixel(xc + x, yc - y, color);
        oled_draw_pixel(xc - x, yc - y, color);
        oled_draw_pixel(xc + y, yc + x, color);
        oled_draw_pixel(xc - y, yc + x, color);
        oled_draw_pixel(xc + y, yc - x, color);
        oled_draw_pixel(xc - y, yc - x, color);
        x++;
        if (d > 0) {
            y--;
            d = d + 4 * (x - y) + 10;
        } else {
            d = d + 4 * x + 6;
        }
    }
}

// Background Animation Render Loop (20 FPS)
static void oled_animation_task(void *pvParameters)
{
    uint32_t frame_count = 0;
    while (1) {
        if (xSemaphoreTake(fb_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            oled_clear_buffer();

            // Draw status bar at the top (underlined)
            oled_draw_line(0, 10, 127, 10, true);
            oled_draw_string(2, 2, "AI VOICE ASSISTANT");

            // Check if there is an active custom message override
            if (is_custom_message_active) {
                oled_draw_string(2, 16, custom_title);
                // Simple wrapping logic for the body message (3 lines of 21 chars)
                char line[22];
                int len = strlen(custom_message);
                for (int i = 0; i < 3; i++) {
                    int offset = i * 21;
                    if (offset >= len) break;
                    int chunk = len - offset;
                    if (chunk > 21) chunk = 21;
                    strncpy(line, custom_message + offset, chunk);
                    line[chunk] = '\0';
                    oled_draw_string(2, 28 + (i * 10), line);
                }
            } else {
                // Render state-specific screen content and animations
                switch (current_state) {
                    case OLED_STATE_BOOT: {
                        oled_draw_string(15, 20, "System Booting...");
                        // Animate a loading bar
                        int progress = (frame_count % 30) * 4; // 0 to 120 pixels
                        oled_draw_line(4, 40, 123, 40, true);
                        oled_draw_line(4, 46, 123, 46, true);
                        oled_draw_line(4, 40, 4, 46, true);
                        oled_draw_line(123, 40, 123, 46, true);
                        for (int px = 6; px < 6 + progress && px < 122; px++) {
                            oled_draw_line(px, 42, px, 44, true);
                        }
                        break;
                    }
                    case OLED_STATE_WIFI_PORTAL: {
                        oled_draw_string(20, 18, "[WIFI SETUP]");
                        oled_draw_string(5, 32, "AP: ESP32-Assistant");
                        oled_draw_string(5, 45, "IP: 192.168.4.1");
                        
                        // Flashing connection dot
                        if ((frame_count / 10) % 2 == 0) {
                            oled_draw_circle(115, 22, 3, true);
                        }
                        break;
                    }
                    case OLED_STATE_WIFI_CONNECTING: {
                        oled_draw_string(12, 20, "Connecting WiFi");
                        int dots = (frame_count / 8) % 4;
                        if (dots == 1) oled_draw_string(45, 34, ".");
                        else if (dots == 2) oled_draw_string(45, 34, "..");
                        else if (dots == 3) oled_draw_string(45, 34, "...");
                        break;
                    }
                    case OLED_STATE_READY: {
                        oled_draw_string(30, 24, "Assistant");
                        oled_draw_string(40, 36, "READY");
                        // Small microphone icon
                        oled_draw_circle(64, 52, 4, true);
                        oled_draw_line(64, 56, 64, 60, true);
                        oled_draw_line(60, 60, 68, 60, true);
                        break;
                    }
                    case OLED_STATE_LISTENING: {
                        oled_draw_string(32, 16, "Listening...");
                        // Bouncing microphone waveform based on mic_level
                        int mid_y = 44;
                        float val = current_mic_level * 25.0f; // Scale up to 25px max height
                        if (val < 2.0f) val = 2.0f; // Minimum line size

                        for (int i = 10; i < 118; i += 8) {
                            float offset = sinf((float)frame_count * 0.2f + (float)i * 0.05f);
                            int height = (int)(val * (0.3f + 0.7f * fabs(offset)));
                            oled_draw_line(i, mid_y - height / 2, i, mid_y + height / 2, true);
                        }
                        break;
                    }
                    case OLED_STATE_TRANSCRIBING: {
                        oled_draw_string(14, 24, "TRANSCRIBING");
                        // Animated dots indicator
                        int dots_t = (frame_count / 8) % 4;
                        if (dots_t == 1) oled_draw_string(56, 38, ".");
                        else if (dots_t == 2) oled_draw_string(53, 38, "..");
                        else if (dots_t == 3) oled_draw_string(50, 38, "...");
                        break;
                    }
                    case OLED_STATE_THINKING: {
                        oled_draw_string(32, 16, "Thinking...");
                        // Rotating loader animation
                        int xc = 64, yc = 42, r = 10;
                        float angle = (float)frame_count * 0.2f;
                        int x_tip = xc + (int)(r * cosf(angle));
                        int y_tip = yc + (int)(r * sinf(angle));
                        oled_draw_circle(xc, yc, r, true);
                        oled_draw_line(xc, yc, x_tip, y_tip, true);
                        break;
                    }
                    case OLED_STATE_GENERATING_VOICE: {
                        oled_draw_string(23, 24, "GENERATING");
                        oled_draw_string(35, 38, "VOICE...");
                        break;
                    }
                    case OLED_STATE_SPEAKING: {
                        oled_draw_string(32, 16, "Speaking...");
                        // Audio spectrum simulator
                        int mid_y = 48;
                        for (int i = 15; i < 115; i += 6) {
                            int height = (int)(15.0f * (0.2f + 0.8f * (float)rand() / (float)RAND_MAX));
                            oled_draw_line(i, mid_y - height / 2, i, mid_y + height / 2, true);
                        }
                        break;
                    }
                    case OLED_STATE_OTA: {
                        oled_draw_string(25, 18, "FIRMWARE OTA");
                        oled_draw_string(10, 32, "Downloading update");
                        // Flash warning
                        if ((frame_count / 6) % 2 == 0) {
                            oled_draw_string(8, 48, "DO NOT POWER OFF!");
                        }
                        break;
                    }
                    case OLED_STATE_ERROR: {
                        oled_draw_string(45, 16, "ERROR");
                        // Error Warning Icon (Triangle)
                        oled_draw_line(20, 48, 30, 28, true);
                        oled_draw_line(30, 28, 40, 48, true);
                        oled_draw_line(20, 48, 40, 48, true);
                        oled_draw_pixel(30, 38, true);
                        oled_draw_pixel(30, 44, true);

                        oled_draw_string(48, 34, "Failed API");
                        break;
                    }
                }
            }

            // Flush buffer to SSD1306
            ssd1306_write_data(frame_buffer, FB_SIZE);
            xSemaphoreGive(fb_mutex);
        }
        frame_count++;
        vTaskDelay(pdMS_TO_TICKS(50)); // ~20 FPS
    }
}

esp_err_t oled_init(void)
{
    ESP_LOGI(TAG, "Initializing SSD1306 OLED...");

    // Create frame buffer mutex
    fb_mutex = xSemaphoreCreateMutex();
    if (!fb_mutex) return ESP_ERR_NO_MEM;

    // 1. Initialize I2C Master Bus
    i2c_master_bus_config_t i2c_mst_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_PORT,
        .scl_io_num = SCL_GPIO,
        .sda_io_num = SDA_GPIO,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus_handle;
    esp_err_t err = i2c_new_master_bus(&i2c_mst_config, &bus_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize I2C master bus: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Scanning I2C Bus on SDA=%d SCL=%d...", SDA_GPIO, SCL_GPIO);
    bool found_device = false;
    for (uint8_t addr = 1; addr < 127; addr++) {
        esp_err_t res = i2c_master_probe(bus_handle, addr, 100);
        if (res == ESP_OK) {
            ESP_LOGI(TAG, "Found I2C device at address 0x%02x", addr);
            found_device = true;
        }
    }
    if (!found_device) {
        ESP_LOGW(TAG, "No I2C devices found on bus!");
    }

    // 2. Add SSD1306 device to bus
    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = SSD1306_I2C_ADDR,
        .scl_speed_hz = 400000,
    };
    err = i2c_master_bus_add_device(bus_handle, &dev_config, &oled_dev_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add I2C device: %s", esp_err_to_name(err));
        return err;
    }

    // 3. Initialize SSD1306 using commands sequence
    vTaskDelay(pdMS_TO_TICKS(100)); // Wait for SSD1306 power stabilization

    uint8_t init_cmds[] = {
        0xAE,         // Display OFF
        0xD5, 0x80,   // Set Display Clock Divide Ratio / Oscillator Frequency
        0xA8, 0x3F,   // Set Multiplex Ratio (1 to 64)
        0xD3, 0x00,   // Set Display Offset (0)
        0x40,         // Set Start Line (0)
        0x8D, 0x14,   // Enable Charge Pump
        0x20, 0x00,   // Set Memory Addressing Mode (Horizontal)
        0xA1,         // Set Segment Re-map (COL127 to SEG0)
        0xC8,         // Set COM Output Scan Direction (Remapped)
        0xDA, 0x12,   // Set COM Pins Hardware Configuration
        0x81, 0x7F,   // Set Contrast (128)
        0xD9, 0xF1,   // Set Pre-charge Period
        0xDB, 0x40,   // Set VCOMH Deselect Level
        0xA4,         // Output follows RAM content
        0xA6,         // Normal Display
        0xAF          // Display ON
    };

    for (size_t i = 0; i < sizeof(init_cmds); i++) {
        err = ssd1306_write_cmd(init_cmds[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "SSD1306 write cmd index %d failed: %s", i, esp_err_to_name(err));
            return err;
        }
    }

    // Horizontal addressing setup
    ssd1306_write_cmd(0x21); // Set column address
    ssd1306_write_cmd(0);    // Start address
    ssd1306_write_cmd(127);  // End address
    ssd1306_write_cmd(0x22); // Set page address
    ssd1306_write_cmd(0);    // Start page
    ssd1306_write_cmd(7);    // End page

    oled_clear_buffer();
    ssd1306_write_data(frame_buffer, FB_SIZE);

    ESP_LOGI(TAG, "SSD1306 Display initialized successfully.");

    // Start background render task
    xTaskCreatePinnedToCore(oled_animation_task, "oled_anim_task", 3072, NULL, 5, &oled_task_handle, 1);

    return ESP_OK;
}

void oled_set_state(oled_state_t state)
{
    if (xSemaphoreTake(fb_mutex, portMAX_DELAY) == pdTRUE) {
        current_state = state;
        is_custom_message_active = false; // Reset message override on state change
        xSemaphoreGive(fb_mutex);
    }
}

void oled_set_custom_message(const char* title, const char* msg)
{
    if (xSemaphoreTake(fb_mutex, portMAX_DELAY) == pdTRUE) {
        strncpy(custom_title, title, sizeof(custom_title) - 1);
        custom_title[sizeof(custom_title) - 1] = '\0';
        strncpy(custom_message, msg, sizeof(custom_message) - 1);
        custom_message[sizeof(custom_message) - 1] = '\0';
        is_custom_message_active = true;
        xSemaphoreGive(fb_mutex);
    }
}

void oled_set_mic_level(float level)
{
    if (level < 0.0f) level = 0.0f;
    if (level > 1.0f) level = 1.0f;
    
    // Low-pass filter to smooth mic level changes on display
    current_mic_level = 0.7f * current_mic_level + 0.3f * level;
}
