#include "wifi_manager.h"
#include "storage.h"
#include "oled_display.h"

#include <string.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_http_server.h>
#include <lwip/sys.h>
#include <lwip/netdb.h>
#include <lwip/api.h>
#include <lwip/sockets.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>

static const char *TAG = "WIFI_MGR";

// Event Group bits
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
static EventGroupHandle_t wifi_event_group;
static int s_retry_num = 0;
static const int MAX_STA_RETRIES = 5;

static httpd_handle_t web_server = NULL;
static bool is_connected = false;
static TaskHandle_t dns_task_handle = NULL;
static int dns_socket = -1;

// Embedded Setup Portal HTML
static const char* setup_html = 
"<!DOCTYPE html>"
"<html>"
"<head>"
"<meta name='viewport' content='width=device-width, initial-scale=1.0'>"
"<title>Voice Assistant Setup</title>"
"<style>"
"body { font-family: 'Segoe UI', Tahoma, sans-serif; background: linear-gradient(135deg, #13111C 0%, #07060A 100%); color: #E2E1E6; margin: 0; display: flex; justify-content: center; align-items: center; min-height: 100vh; }"
".container { background: rgba(25, 22, 38, 0.85); backdrop-filter: blur(10px); padding: 30px; border-radius: 16px; box-shadow: 0 8px 32px 0 rgba(0, 0, 0, 0.5); width: 100%; max-width: 400px; border: 1px solid rgba(255, 255, 255, 0.08); box-sizing: border-box; }"
"h2 { text-align: center; color: #9C8CFF; margin-bottom: 24px; font-weight: 600; font-size: 22px; letter-spacing: 0.5px; }"
".form-group { margin-bottom: 20px; }"
"label { display: block; margin-bottom: 8px; font-size: 13px; color: #A9A8B3; text-transform: uppercase; letter-spacing: 0.5px; }"
"input[type='text'], input[type='password'] { width: 100%; padding: 12px; border: 1px solid rgba(255, 255, 255, 0.12); border-radius: 8px; background: rgba(10, 8, 16, 0.6); color: #FFF; box-sizing: border-box; transition: all 0.3s ease; }"
"input[type='text']:focus, input[type='password']:focus { outline: none; border-color: #9C8CFF; box-shadow: 0 0 8px rgba(156, 140, 255, 0.4); }"
"button { width: 100%; padding: 14px; background: linear-gradient(90deg, #7C66FF 0%, #9C8CFF 100%); color: white; border: none; border-radius: 8px; font-weight: bold; cursor: pointer; transition: opacity 0.3s; margin-top: 10px; font-size: 14px; letter-spacing: 0.5px; }"
"button:hover { opacity: 0.9; }"
".footer { text-align: center; font-size: 11px; color: #62606E; margin-top: 24px; }"
"</style>"
"</head>"
"<body>"
"<div class='container'>"
"<h2>ASSISTANT CONFIG</h2>"
"<form action='/save' method='GET'>"
"<div class='form-group'>"
"<label>WiFi SSID</label>"
"<input type='text' name='ssid' required placeholder='Enter WiFi SSID'>"
"</div>"
"<div class='form-group'>"
"<label>WiFi Password</label>"
"<input type='password' name='pass' placeholder='Enter WiFi Password'>"
"</div>"
"<div class='form-group'>"
"<label>DeepSeek API Key</label>"
"<input type='password' name='ds_key' placeholder='sk-xxxxxx...'>"
"</div>"
"<div class='form-group'>"
"<label>Telegram Bot Token</label>"
"<input type='password' name='tg_token' placeholder='123456:ABC-xxx...'>"
"</div>"
"<button type='submit'>SAVE CONFIGURATION</button>"
"</form>"
"<div class='footer'>ESP32-S3 Voice Assistant Setup Portal</div>"
"</div>"
"</body>"
"</html>";

// WiFi and IP Event Handler
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < MAX_STA_RETRIES) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Retrying WiFi connection (%d/%d)...", s_retry_num, MAX_STA_RETRIES);
        } else {
            xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        is_connected = true;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

// Redirect client to http://192.168.4.1 for Captive Portal detection
static esp_err_t captive_redirect_handler(httpd_req_t *req)
{
    ESP_LOGD(TAG, "Redirecting Captive request: %s", req->uri);
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// Serve standard index setup page
static esp_err_t root_get_handler(httpd_req_t *req)
{
    // Check if host matches our gateway IP
    char host_hdr[64] = {0};
    if (httpd_req_get_hdr_value_str(req, "Host", host_hdr, sizeof(host_hdr)) == ESP_OK) {
        if (strstr(host_hdr, "192.168.4.1") == NULL) {
            return captive_redirect_handler(req);
        }
    }
    
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, setup_html, HTTPD_RESP_USE_SIZEOF);
    return ESP_OK;
}

// Parse URL Query strings helper
static esp_err_t get_query_key_value(const char* query, const char* key, char* buffer, size_t max_len)
{
    char key_eq[32];
    snprintf(key_eq, sizeof(key_eq), "%s=", key);
    char* pos = strstr(query, key_eq);
    if (!pos) return ESP_ERR_NOT_FOUND;
    pos += strlen(key_eq);
    char* end = strchr(pos, '&');
    size_t len = end ? (size_t)(end - pos) : strlen(pos);
    if (len >= max_len) len = max_len - 1;
    strncpy(buffer, pos, len);
    buffer[len] = '\0';
    
    // Simple URL decoding
    char temp[max_len];
    size_t i = 0, j = 0;
    while (buffer[i] != '\0' && j < max_len - 1) {
        if (buffer[i] == '%') {
            if (buffer[i+1] != '\0' && buffer[i+2] != '\0') {
                char hex[3] = { buffer[i+1], buffer[i+2], '\0' };
                temp[j++] = (char)strtol(hex, NULL, 16);
                i += 3;
            } else {
                temp[j++] = buffer[i++];
            }
        } else if (buffer[i] == '+') {
            temp[j++] = ' ';
            i++;
        } else {
            temp[j++] = buffer[i++];
        }
    }
    temp[j] = '\0';
    strcpy(buffer, temp);
    return ESP_OK;
}

// Handle Configuration Save Endpoint
static esp_err_t save_get_handler(httpd_req_t *req)
{
    char query[256] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Query string empty");
        return ESP_FAIL;
    }

    char ssid[32] = {0};
    char pass[64] = {0};
    char ds_key[96] = {0};
    char tg_token[64] = {0};

    get_query_key_value(query, "ssid", ssid, sizeof(ssid));
    get_query_key_value(query, "pass", pass, sizeof(pass));
    get_query_key_value(query, "ds_key", ds_key, sizeof(ds_key));
    get_query_key_value(query, "tg_token", tg_token, sizeof(tg_token));

    ESP_LOGI(TAG, "Saving configurations to NVS: SSID=%s", ssid);

    if (strlen(ssid) > 0) {
        storage_save_string("wifi_ssid", ssid);
        storage_save_string("wifi_pass", pass);
        if (strlen(ds_key) > 0) storage_save_string("deepseek_key", ds_key);
        if (strlen(tg_token) > 0) storage_save_string("telegram_token", tg_token);

        const char* success_resp = 
        "<html><head><style>body { background:#07060A; color:#E2E1E6; font-family:sans-serif; text-align:center; padding-top:100px; }</style></head>"
        "<body><h2>Configuration Saved Successfully!</h2><p>Rebooting Assistant device... Please connect to your WiFi network.</p></body></html>";
        httpd_resp_send(req, success_resp, HTTPD_RESP_USE_SIZEOF);

        vTaskDelay(pdMS_TO_TICKS(1500));
        esp_restart();
    } else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID is required");
    }

    return ESP_OK;
}

// Start HTTP web server
static void start_web_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_len = 1024;
    config.ctrl_port = 32768; // Change ctrl port to avoid standard conflicts
    
    ESP_LOGI(TAG, "Starting web server on port %d...", config.server_port);
    if (httpd_start(&web_server, &config) == ESP_OK) {
        httpd_uri_t save_uri = {
            .uri      = "/save",
            .method   = HTTP_GET,
            .handler  = save_get_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(web_server, &save_uri);

        // Standard Root serves index or redirects
        httpd_uri_t root_uri = {
            .uri      = "/",
            .method   = HTTP_GET,
            .handler  = root_get_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(web_server, &root_uri);

        // Wildcard fallback redirects unknown requests to 192.168.4.1 (Captive Portal trigger)
        httpd_uri_t redirect_uri = {
            .uri      = "/*",
            .method   = HTTP_GET,
            .handler  = captive_redirect_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(web_server, &redirect_uri);
    }
}

// DNS UDP Server Task (Listens to queries and redirects to 192.168.4.1)
static void dns_server_task(void *pvParameters)
{
    uint8_t rx_buffer[256];
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_addr_len = sizeof(client_addr);

    dns_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (dns_socket < 0) {
        ESP_LOGE(TAG, "Unable to create DNS socket");
        vTaskDelete(NULL);
        return;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(53); // DNS standard port
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(dns_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "DNS socket bind failed");
        close(dns_socket);
        dns_socket = -1;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "DNS Redirect Server started on port 53.");

    while (1) {
        int len = recvfrom(dns_socket, rx_buffer, sizeof(rx_buffer), 0, (struct sockaddr *)&client_addr, &client_addr_len);
        if (len < 0) {
            ESP_LOGD(TAG, "DNS recvfrom failed");
            break;
        }

        // Basic parsing of DNS Query
        if (len > 12) {
            // Modify packet to form a DNS Response
            rx_buffer[2] |= 0x80; // QR = 1 (Response)
            rx_buffer[3] = 0x80;  // AA = 1, RA = 0, RCODE = 0 (No error)
            
            // Set Answer count to 1 (Question count is already 1)
            rx_buffer[6] = 0;
            rx_buffer[7] = 1; 

            // Find end of Question name
            int idx = 12;
            while (rx_buffer[idx] != 0 && idx < len) {
                idx += rx_buffer[idx] + 1;
            }
            idx += 5; // Skip Type and Class (4 bytes) + 0x00 terminal

            if (idx + 16 < sizeof(rx_buffer)) {
                // Add Answer Record
                rx_buffer[idx++] = 0xC0; // Compression offset pointer to name
                rx_buffer[idx++] = 0x0C; // Points to start of query name (offset 12)

                rx_buffer[idx++] = 0x00; // Type A
                rx_buffer[idx++] = 0x01;
                rx_buffer[idx++] = 0x00; // Class IN
                rx_buffer[idx++] = 0x01;

                // TTL = 60 seconds
                rx_buffer[idx++] = 0x00;
                rx_buffer[idx++] = 0x00;
                rx_buffer[idx++] = 0x00;
                rx_buffer[idx++] = 0x3C;

                // Data Length = 4
                rx_buffer[idx++] = 0x00;
                rx_buffer[idx++] = 0x04;

                // IP Address: 192.168.4.1
                rx_buffer[idx++] = 192;
                rx_buffer[idx++] = 168;
                rx_buffer[idx++] = 4;
                rx_buffer[idx++] = 1;

                sendto(dns_socket, rx_buffer, idx, 0, (struct sockaddr *)&client_addr, client_addr_len);
            }
        }
    }

    if (dns_socket >= 0) {
        close(dns_socket);
        dns_socket = -1;
    }
    vTaskDelete(NULL);
}

// Initialize WiFi AP Mode
static void start_access_point(void)
{
    ESP_LOGI(TAG, "Starting Access Point configuration portal...");
    oled_set_state(OLED_STATE_WIFI_PORTAL);

    esp_netif_t* ap_netif = esp_netif_create_default_wifi_ap();

    // Set AP IP gateway to 192.168.4.1
    esp_netif_ip_info_t ip_info;
    memset(&ip_info, 0, sizeof(ip_info));
    IP4_ADDR(&ip_info.ip, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.gw, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
    esp_netif_dhcps_stop(ap_netif);
    esp_netif_set_ip_info(ap_netif, &ip_info);
    esp_netif_dhcps_start(ap_netif);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = "ESP32-S3-Assistant",
            .ssid_len = strlen("ESP32-S3-Assistant"),
            .channel = 1,
            .max_connection = 4,
            .authmode = WIFI_AUTH_OPEN // Open auth for easy portal onboarding
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "AP WiFi initialized. SSID: ESP32-S3-Assistant");

    // Launch configuration server and DNS redirect server
    start_web_server();
    xTaskCreate(dns_server_task, "dns_server", 3072, NULL, 4, &dns_task_handle);
}

esp_err_t wifi_manager_init(bool wait_for_connect)
{
    wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    char ssid[32] = {0};
    char pass[64] = {0};

    // Check NVS
    bool has_creds = (storage_read_string("wifi_ssid", ssid, sizeof(ssid)) == ESP_OK);
    if (has_creds) {
        storage_read_string("wifi_pass", pass, sizeof(pass));
    }

    if (has_creds && strlen(ssid) > 0) {
        // -----------------
        // Connect in Station Mode
        // -----------------
        ESP_LOGI(TAG, "Connecting to SSID: %s...", ssid);
        oled_set_state(OLED_STATE_WIFI_CONNECTING);

        esp_netif_create_default_wifi_sta();
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));

        esp_event_handler_instance_t instance_any_id;
        esp_event_handler_instance_t instance_got_ip;
        ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                            ESP_EVENT_ANY_ID,
                                                            &wifi_event_handler,
                                                            NULL,
                                                            &instance_any_id));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                            IP_EVENT_STA_GOT_IP,
                                                            &wifi_event_handler,
                                                            NULL,
                                                            &instance_got_ip));

        wifi_config_t wifi_sta_config = {0};
        strcpy((char*)wifi_sta_config.sta.ssid, ssid);
        strcpy((char*)wifi_sta_config.sta.password, pass);
        wifi_sta_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_sta_config));
        ESP_ERROR_CHECK(esp_wifi_start());

        if (wait_for_connect) {
            EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
                                                   WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                                   pdFALSE,
                                                   pdFALSE,
                                                   portMAX_DELAY);
            if (bits & WIFI_CONNECTED_BIT) {
                ESP_LOGI(TAG, "Connected successfully.");
                oled_set_state(OLED_STATE_READY);
                return ESP_OK;
            } else {
                ESP_LOGW(TAG, "Failed to connect. Falling back to Access Point...");
                // Clean up STA and start AP
                esp_wifi_stop();
                esp_wifi_deinit();
                esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip);
                esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id);
                // Spawning AP
                start_access_point();
            }
        }
    } else {
        // No credentials found, startup AP mode straight away
        start_access_point();
    }

    return ESP_OK;
}

bool wifi_manager_is_connected(void)
{
    return is_connected;
}
