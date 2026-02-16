#include "esp_log.h"
#include "esp_spiffs.h"
#include "web_server.h"
#include "config_manager.h" // Required for app_config_t
#include "esp_wifi.h"
#include "esp_http_server.h"
#include "esp_mac.h" // Required for MACSTR and MAC2STR
#include <fcntl.h>
#include <unistd.h> // Required for read() and close()
#include <dirent.h> // Required for directory listing

static const char *TAG = "WEB_SERVER";

esp_err_t init_spiffs(void)
{
    ESP_LOGI(TAG, "Initializing SPIFFS");

    esp_vfs_spiffs_conf_t conf = {
      .base_path = "/spiffs",
      .partition_label = "storage",
      .max_files = 5,   // This is the maximum number of files that can be open at the same time.
      .format_if_mount_failed = true
    };

    // Use a wrapper function to register and mount SPIFFS filesystem.
    // Note: esp_vfs_spiffs_register is an all-in-one convenience function.
    esp_err_t ret = esp_vfs_spiffs_register(&conf);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount or format filesystem");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to find SPIFFS partition");
        } else {
            ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
        }
        return ret;
    }

    size_t total = 0, used = 0;
    ret = esp_spiffs_info(conf.partition_label, &total, &used);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s)", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Partition size: total: %d, used: %d", total, used);
    }

    return ESP_OK;
}

void list_spiffs_files(const char* path) {
    ESP_LOGI(TAG, "Listing files in %s", path);
    DIR *dir = opendir(path);
    if (!dir) {
        ESP_LOGE(TAG, "Failed to open directory: %s", path);
        return;
    }
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        ESP_LOGI(TAG, "Found file: %s", ent->d_name);
    }
    closedir(dir);
}

// --- Web Server ---

/* An HTTP GET handler */
static esp_err_t root_get_handler(httpd_req_t *req)
{
    char filepath[576];
    
    // Handle root path "/" specially
    if (strcmp(req->uri, "/") == 0) {
        snprintf(filepath, sizeof(filepath), "/spiffs/index.html");
    } else {
        // For other paths, prepend "/spiffs/"
        snprintf(filepath, sizeof(filepath), "/spiffs%s", req->uri);
    }

    FILE* file = fopen(filepath, "r");
    if (file == NULL) {
        ESP_LOGE(TAG, "Failed to open file : %s", filepath);
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Serving file: %s", filepath);
    
    // Set content type based on file extension
    if (strstr(filepath, ".css") != NULL) {
        httpd_resp_set_type(req, "text/css");
    } else if (strstr(filepath, ".js") != NULL) {
        httpd_resp_set_type(req, "application/javascript");
    } else if (strstr(filepath, ".png") != NULL) {
        httpd_resp_set_type(req, "image/png");
    } else if (strstr(filepath, ".jpg") != NULL || strstr(filepath, ".jpeg") != NULL) {
        httpd_resp_set_type(req, "image/jpeg");
    } else if (strstr(filepath, ".ico") != NULL) {
        httpd_resp_set_type(req, "image/x-icon");
    } else {
        httpd_resp_set_type(req, "text/html");
    }

    char *chunk = (char*)malloc(1024);
    size_t read_bytes;
    while ((read_bytes = fread(chunk, 1, 1024, file)) > 0) {
        if (httpd_resp_send_chunk(req, chunk, read_bytes) != ESP_OK) {
            fclose(file);
            ESP_LOGE(TAG, "File sending failed!");
            free(chunk);
            return ESP_FAIL;
        }
    }

    fclose(file);
    free(chunk);
    httpd_resp_send_chunk(req, NULL, 0); // Finalize response
    return ESP_OK;
}

/* An HTTP GET handler for retrieving current/default configuration */
static esp_err_t config_get_handler(httpd_req_t *req)
{
    // Load current config, or use defaults if not available
    app_config_t current_config = {};
    esp_err_t load_result = load_config(&current_config);
    
    // If no config is saved, use default values
    if (load_result != ESP_OK) {
        get_default_config(&current_config);
    }
    
    // Create JSON response (don't include password for security)
    char response[512];
    snprintf(response, sizeof(response),
             "{"
             "\"ssid\":\"%s\","
             "\"mqtt_host\":\"%s\""
             "}",
             current_config.wifi_ssid,
             current_config.mqtt_host);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, response);
    
    return ESP_OK;
}

/* An HTTP POST handler for saving configuration */
static esp_err_t save_post_handler(httpd_req_t *req)
{
    char buf[1024];  // Increased buffer size to handle form data
    int remaining = req->content_len;  // Total content length

    app_config_t new_config = {};
    
    // Initialize the config structure
    memset(&new_config, 0, sizeof(app_config_t));

    // Read the entire content of the POST request
    char *post_data = (char *)calloc(1, remaining + 1);
    if (!post_data) {
        ESP_LOGE(TAG, "Failed to allocate memory for POST data");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
        return ESP_FAIL;
    }

    int pos = 0;
    while (remaining > 0) {
        int recv_len = httpd_req_recv(req, buf, MIN(remaining, sizeof(buf)));
        if (recv_len <= 0) {
            if (recv_len == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            free(post_data);
            return ESP_FAIL;
        }

        memcpy(post_data + pos, buf, recv_len);
        pos += recv_len;
        remaining -= recv_len;
    }
    
    // Null-terminate the received data
    post_data[pos] = '\0';

    ESP_LOGI(TAG, "Received POST data: %s", post_data);

    // Parse the form data (format: key1=value1&key2=value2)
    char *temp_data = strdup(post_data); // Make a copy for tokenizing
    if (!temp_data) {
        ESP_LOGE(TAG, "Failed to duplicate POST data for parsing");
        free(post_data);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
        return ESP_FAIL;
    }

    // Log the raw pairs before parsing
    ESP_LOGI(TAG, "Raw POST data for parsing: %s", temp_data);
    
    // Create a copy of the data for parsing to avoid modifying the original
    char *parse_copy = strdup(temp_data);
    if (!parse_copy) {
        ESP_LOGE(TAG, "Failed to allocate memory for parsing");
        free(post_data);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
        return ESP_FAIL;
    }
    
    // Split by '&' to get key-value pairs
    char *token = strtok(parse_copy, "&");
    while (token != NULL) {
        ESP_LOGI(TAG, "Processing pair: %s", token);
        
        // Find the position of '=' to split key and value
        char *separator = strchr(token, '=');
        if (separator != NULL) {
            *separator = '\0'; // Temporarily terminate the key
            char *key = token;
            char *value = separator + 1; // Value starts after '='
            
            ESP_LOGI(TAG, "Extracted key: '%s', value: '%s'", key, value);
            
            // URL decode the value
            char decoded_value[256];
            // Create a temporary string with the key=value pair to decode
            char temp_pair[512];
            snprintf(temp_pair, sizeof(temp_pair), "%s=%s", key, value);
            
            // Use httpd_query_key_value to decode the value
            size_t decoded_len = httpd_query_key_value(temp_pair, key, decoded_value, sizeof(decoded_value));
            if (decoded_len != (size_t)-1) {
                // If decoding was successful, the decoded value is in decoded_value
                ESP_LOGI(TAG, "Decoded value: '%s'", decoded_value);
            } else {
                // If decoding failed, use the original value
                strncpy(decoded_value, value, sizeof(decoded_value) - 1);
                decoded_value[sizeof(decoded_value) - 1] = '\0';
                ESP_LOGI(TAG, "Using original value: '%s'", decoded_value);
            }
            
            ESP_LOGI(TAG, "Final parsed: %s = %s", key, decoded_value);
            
            if (strcmp(key, "ssid") == 0) {
                strncpy(new_config.wifi_ssid, decoded_value, sizeof(new_config.wifi_ssid) - 1);
                new_config.wifi_ssid[sizeof(new_config.wifi_ssid) - 1] = '\0';
                ESP_LOGI(TAG, "Saved SSID: '%s'", new_config.wifi_ssid);
            } else if (strcmp(key, "password") == 0) {
                strncpy(new_config.wifi_password, decoded_value, sizeof(new_config.wifi_password) - 1);
                new_config.wifi_password[sizeof(new_config.wifi_password) - 1] = '\0';
                ESP_LOGI(TAG, "Saved Password: '%s'", new_config.wifi_password);
            } else if (strcmp(key, "mqtt_host") == 0) {
                strncpy(new_config.mqtt_host, decoded_value, sizeof(new_config.mqtt_host) - 1);
                new_config.mqtt_host[sizeof(new_config.mqtt_host) - 1] = '\0';
                ESP_LOGI(TAG, "Saved MQTT Host: '%s'", new_config.mqtt_host);
            }
        } else {
            ESP_LOGW(TAG, "Invalid key-value pair (no '=' found): '%s'", token);
        }
        
        token = strtok(NULL, "&");
    }
    
    free(parse_copy); // Free the copied string

    free(temp_data); // Free the duplicated string

    // Validate that required fields are present
    if (strlen(new_config.wifi_ssid) == 0) {
        ESP_LOGE(TAG, "SSID is required");
        free(post_data);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID is required");
        return ESP_FAIL;
    }

    // Save the new configuration
    esp_err_t save_result = save_config(&new_config);
    if (save_result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save configuration: %s", esp_err_to_name(save_result));
        free(post_data);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save configuration");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Configuration saved successfully. SSID: %s, Password: [HIDDEN], MQTT Host: %s", new_config.wifi_ssid, new_config.mqtt_host);

    // Send success response
    httpd_resp_set_type(req, "application/json");
    const char* response = "{\"status\":\"success\",\"message\":\"Configuration saved and device will reconnect to WiFi\"}";
    httpd_resp_sendstr(req, response);

    free(post_data);

    // Instead of restarting here, we'll let the main loop handle the transition
    ESP_LOGI(TAG, "Configuration saved successfully. Device will reconnect to WiFi...");

    // Notify the main application that configuration has been saved
    extern void on_config_saved(); // Forward declaration
    on_config_saved();

    return ESP_OK;
}

/* An HTTP GET handler for the root path */
static esp_err_t root_handler(httpd_req_t *req)
{
    // For root path, redirect to index.html
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/index.html");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static const httpd_uri_t root = {
    .uri       = "/",
    .method    = HTTP_GET,
    .handler   = root_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t index_html = {
    .uri       = "/index.html",
    .method    = HTTP_GET,
    .handler   = root_get_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t wildcard = {
    .uri       = "/*",
    .method    = HTTP_GET,
    .handler   = root_get_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t config_handler = {
    .uri       = "/config",
    .method    = HTTP_GET,
    .handler   = config_get_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t save = {
    .uri       = "/save",
    .method    = HTTP_POST,
    .handler   = save_post_handler,
    .user_ctx  = NULL
};

esp_err_t start_web_server(httpd_handle_t *server)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;

    ESP_LOGI(TAG, "Starting server on port: '%d'", config.server_port);
    if (httpd_start(server, &config) == ESP_OK) {
        ESP_LOGI(TAG, "Registering URI handlers");
        httpd_register_uri_handler(*server, &root);      // Handle root path
        httpd_register_uri_handler(*server, &index_html);     // Handle index.html specifically
        httpd_register_uri_handler(*server, &wildcard);  // Handle all other GET requests
        httpd_register_uri_handler(*server, &config_handler);    // Handle GET to /config
        httpd_register_uri_handler(*server, &save);      // Handle POST to /save
        return ESP_OK;
    }

    ESP_LOGE(TAG, "Error starting server!");
    return ESP_FAIL;
}

esp_err_t stop_web_server(httpd_handle_t server)
{
    if (server == NULL) {
        ESP_LOGW(TAG, "Server handle is NULL, nothing to stop");
        return ESP_OK;
    }
    
    esp_err_t err = httpd_stop(server);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Server stopped successfully");
    } else {
        ESP_LOGE(TAG, "Error stopping server: %s", esp_err_to_name(err));
    }
    return err;
}

// --- Provisioning AP ---

#define PROV_AP_SSID "WhiteList-Detector-Setup"

static void wifi_event_handler_ap(void* arg, esp_event_base_t event_base,
                                    int32_t event_id, void* event_data)
{
    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
        ESP_LOGI(TAG, "station " MACSTR " join, AID=%d",
                 MAC2STR(event->mac), event->aid);
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
        ESP_LOGI(TAG, "station " MACSTR " leave, AID=%d",
                 MAC2STR(event->mac), event->aid);
    }
}

esp_err_t start_provisioning_ap(httpd_handle_t *server)
{
    ESP_LOGI(TAG, "Starting Provisioning AP...");
    
    // Deinitialize WiFi if it was previously initialized
    esp_err_t err = esp_wifi_deinit();
    if (err != ESP_ERR_WIFI_NOT_INIT) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(err);
    }
    
    // Initialize WiFi for AP mode
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Create AP interface
    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler_ap,
                                                        NULL,
                                                        NULL));

    wifi_config_t wifi_config = {}; // Initialize all members to zero
    strncpy((char*)wifi_config.ap.ssid, PROV_AP_SSID, sizeof(wifi_config.ap.ssid));
    wifi_config.ap.ssid_len = strlen(PROV_AP_SSID);
    wifi_config.ap.channel = 6; // Use channel 6 which is more commonly scanned
    wifi_config.ap.authmode = WIFI_AUTH_OPEN; // Open authentication
    wifi_config.ap.ssid_hidden = 0; // Not hidden
    wifi_config.ap.max_connection = 4;
    wifi_config.ap.beacon_interval = 100; // Standard beacon interval
    wifi_config.ap.pairwise_cipher = WIFI_CIPHER_TYPE_CCMP; // Set cipher type

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Set WiFi TX power to 60% (48 units out of 80 max) for better stability
    ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(48));

    ESP_LOGI(TAG, "Provisioning AP started. SSID: %s, Channel: %d", PROV_AP_SSID, wifi_config.ap.channel);

    // List files in SPIFFS for debugging
    list_spiffs_files("/spiffs");
    
    // Check if index.html exists
    FILE* file = fopen("/spiffs/index.html", "r");
    if (file) {
        ESP_LOGI(TAG, "index.html found in SPIFFS");
        fclose(file);
    } else {
        ESP_LOGE(TAG, "index.html NOT FOUND in SPIFFS");
    }

    return start_web_server(server);
}
