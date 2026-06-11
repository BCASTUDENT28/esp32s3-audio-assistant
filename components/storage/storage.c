#include "storage.h"
#include <esp_log.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <esp_spiffs.h>

static const char *TAG = "STORAGE";
static const char *NVS_NAMESPACE = "app_config";

esp_err_t storage_init(void)
{
    // 1. Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS flash needs erasing, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize NVS: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "NVS Initialized successfully.");

    // 2. Initialize SPIFFS
    ESP_LOGI(TAG, "Mounting SPIFFS partition...");
    esp_vfs_spiffs_conf_t conf = {
        .base_path = STORAGE_MOUNT_POINT,
        .partition_label = "storage",
        .max_files = 5,
        .format_if_mount_failed = true
    };

    ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount or format filesystem");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to find SPIFFS partition 'storage'");
        } else {
            ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
        }
        return ret;
    }

    size_t total = 0, used = 0;
    ret = esp_spiffs_info(conf.partition_label, &total, &used);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "SPIFFS Mounted. Partition size: %d bytes, Used: %d bytes", total, used);
    } else {
        ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s)", esp_err_to_name(ret));
    }

    return ESP_OK;
}

esp_err_t storage_save_string(const char* key, const char* value)
{
    nvs_handle_t my_handle;
    esp_err_t err;

    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &my_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS handle: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_str(my_handle, key, value);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error writing string '%s' to NVS: %s", key, esp_err_to_name(err));
        nvs_close(my_handle);
        return err;
    }

    err = nvs_commit(my_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error committing NVS changes: %s", esp_err_to_name(err));
    }

    nvs_close(my_handle);
    return err;
}

esp_err_t storage_read_string(const char* key, char* buffer, size_t max_len)
{
    nvs_handle_t my_handle;
    esp_err_t err;

    err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &my_handle);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "NVS namespace '%s' doesn't exist yet: %s", NVS_NAMESPACE, esp_err_to_name(err));
        return err;
    }

    size_t required_size;
    err = nvs_get_str(my_handle, key, NULL, &required_size);
    if (err != ESP_OK) {
        nvs_close(my_handle);
        return err;
    }

    if (required_size > max_len) {
        nvs_close(my_handle);
        return ESP_ERR_INVALID_SIZE;
    }

    err = nvs_get_str(my_handle, key, buffer, &required_size);
    nvs_close(my_handle);
    return err;
}

esp_err_t storage_save_int32(const char* key, int32_t value)
{
    nvs_handle_t my_handle;
    esp_err_t err;

    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &my_handle);
    if (err != ESP_OK) return err;

    err = nvs_set_i32(my_handle, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(my_handle);
    }
    nvs_close(my_handle);
    return err;
}

esp_err_t storage_read_int32(const char* key, int32_t* value)
{
    nvs_handle_t my_handle;
    esp_err_t err;

    err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &my_handle);
    if (err != ESP_OK) return err;

    err = nvs_get_i32(my_handle, key, value);
    nvs_close(my_handle);
    return err;
}

bool storage_is_wifi_configured(void)
{
    char ssid[32] = {0};
    char pass[64] = {0};
    
    if (storage_read_string("wifi_ssid", ssid, sizeof(ssid)) == ESP_OK &&
        storage_read_string("wifi_pass", pass, sizeof(pass)) == ESP_OK) {
        return (strlen(ssid) > 0);
    }
    
    return false;
}

void storage_clear_all(void)
{
    nvs_handle_t my_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &my_handle) == ESP_OK) {
        nvs_erase_all(my_handle);
        nvs_commit(my_handle);
        nvs_close(my_handle);
        ESP_LOGI(TAG, "Cleared storage config namespace.");
    }
}
