#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include "esp_err.h"
#include "esp_http_server.h" // Required for httpd_handle_t

/**
 * @brief Initializes the SPIFFS file system.
 *
 * Mounts the partition labeled "storage" to the virtual filesystem at the "/spiffs" path.
 * If the partition is not formatted, it will be formatted automatically.
 *
 * @return ESP_OK on success, otherwise an error code.
 */
esp_err_t init_spiffs(void);

/**
 * @brief Starts the web server for provisioning.
 *
 * @return ESP_OK on success, otherwise an error code.
 */
esp_err_t start_web_server(httpd_handle_t *server);

/**
 * @brief Stops the web server.
 *
 * @param server The server handle to stop.
 * @return ESP_OK on success, otherwise an error code.
 */
esp_err_t stop_web_server(httpd_handle_t server);

/**
 * @brief Starts the Wi-Fi in Access Point mode for provisioning.
 *
 * @param server A pointer to store the server handle.
 * @return ESP_OK on success, otherwise an error code.
 */
esp_err_t start_provisioning_ap(httpd_handle_t *server);

/**
 * @brief Lists files in the specified directory in SPIFFS. (For debugging)
 *
 * @param path The directory path to list.
 */
void list_spiffs_files(const char* path);

#endif // WEB_SERVER_H
