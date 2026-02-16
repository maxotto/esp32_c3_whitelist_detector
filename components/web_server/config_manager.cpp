#include "config_manager.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include <string.h>

#define NVS_KEY_WIFI_SSID "wifi_ssid"
#define NVS_KEY_WIFI_PASS "wifi_pass"
#define NVS_KEY_MQTT_HOST "mqtt_host"

static const char* TAG = "CONFIG_MGR";

esp_err_t save_config(const app_config_t* config) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) opening NVS handle!", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_str(nvs_handle, NVS_KEY_WIFI_SSID, config->wifi_ssid);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write " NVS_KEY_WIFI_SSID ": %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }

    err = nvs_set_str(nvs_handle, NVS_KEY_WIFI_PASS, config->wifi_password);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write " NVS_KEY_WIFI_PASS ": %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }

    err = nvs_set_str(nvs_handle, NVS_KEY_MQTT_HOST, config->mqtt_host);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write " NVS_KEY_MQTT_HOST ": %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }

    err = nvs_commit(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit NVS: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Configuration saved successfully.");
    }

    nvs_close(nvs_handle);
    return err;
}

esp_err_t load_config(app_config_t* config) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(CONFIG_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) opening NVS handle!", esp_err_to_name(err));
        return err;
    }

    // Clear the struct first
    memset(config, 0, sizeof(app_config_t));

    size_t required_size;

    // Read SSID
    err = nvs_get_str(nvs_handle, NVS_KEY_WIFI_SSID, NULL, &required_size);
    if (err == ESP_OK) {
        if (required_size > sizeof(config->wifi_ssid)) {
             ESP_LOGE(TAG, NVS_KEY_WIFI_SSID " in NVS is too large for buffer");
             err = ESP_FAIL;
        } else {
            err = nvs_get_str(nvs_handle, NVS_KEY_WIFI_SSID, config->wifi_ssid, &required_size);
        }
    }
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "Failed to read " NVS_KEY_WIFI_SSID ": %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }

    // Read Password
    err = nvs_get_str(nvs_handle, NVS_KEY_WIFI_PASS, NULL, &required_size);
     if (err == ESP_OK) {
        if (required_size > sizeof(config->wifi_password)) {
             ESP_LOGE(TAG, NVS_KEY_WIFI_PASS " in NVS is too large for buffer");
             err = ESP_FAIL;
        } else {
            err = nvs_get_str(nvs_handle, NVS_KEY_WIFI_PASS, config->wifi_password, &required_size);
        }
    }
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "Failed to read " NVS_KEY_WIFI_PASS ": %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }

    // Read MQTT Host
    err = nvs_get_str(nvs_handle, NVS_KEY_MQTT_HOST, NULL, &required_size);
    if (err == ESP_OK) {
        if (required_size > sizeof(config->mqtt_host)) {
             ESP_LOGE(TAG, NVS_KEY_MQTT_HOST " in NVS is too large for buffer");
             err = ESP_FAIL;
        } else {
            err = nvs_get_str(nvs_handle, NVS_KEY_MQTT_HOST, config->mqtt_host, &required_size);
        }
    }
     if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "Failed to read " NVS_KEY_MQTT_HOST ": %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }

    nvs_close(nvs_handle);
    
    if (err == ESP_ERR_NVS_NOT_FOUND) {
         ESP_LOGW(TAG, "No configuration found in NVS. Using defaults.");
         return ESP_ERR_NVS_NOT_FOUND;
    }
    
    ESP_LOGI(TAG, "Configuration loaded successfully.");
    return ESP_OK;
}
