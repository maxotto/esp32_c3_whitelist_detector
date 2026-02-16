#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include "esp_err.h"

#define CONFIG_NAMESPACE "app_config"

// A structure to hold our application's configuration
typedef struct {
    char wifi_ssid[32];
    char wifi_password[64];
    char mqtt_host[64];
} app_config_t;

/**
 * @brief Saves the application configuration to NVS.
 * 
 * @param config Pointer to the configuration structure to save.
 * @return ESP_OK on success, otherwise an error code.
 */
esp_err_t save_config(const app_config_t* config);

/**
 * @brief Loads the application configuration from NVS.
 * 
 * @param config Pointer to the configuration structure to populate.
 * @return ESP_OK on success, or ESP_ERR_NVS_NOT_FOUND if no config is saved.
 */
esp_err_t load_config(app_config_t* config);

#endif // CONFIG_MANAGER_H
