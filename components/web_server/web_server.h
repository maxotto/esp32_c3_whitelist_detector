#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include "esp_err.h"

/**
 * @brief Initializes the SPIFFS file system.
 * 
 * Mounts the partition labeled "storage" to the virtual filesystem at the "/spiffs" path.
 * If the partition is not formatted, it will be formatted automatically.
 * 
 * @return ESP_OK on success, otherwise an error code.
 */
esp_err_t init_spiffs(void);

#endif // WEB_SERVER_H
