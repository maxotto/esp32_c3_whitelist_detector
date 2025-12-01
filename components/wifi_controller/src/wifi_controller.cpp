#include "wifi_controller.hpp"
#include <cstring>

WiFiController::WiFiController() 
    : state_(ConnectionState::DISCONNECTED), wifi_event_group_(nullptr) {
}

WiFiController::~WiFiController() {
    esp_wifi_stop();
    esp_wifi_deinit();
    if (wifi_event_group_) {
        vEventGroupDelete(wifi_event_group_);
    }
}

bool WiFiController::initialize() {
    // Create event group for WiFi events
    wifi_event_group_ = xEventGroupCreate();
    if (!wifi_event_group_) {
        ESP_LOGE(TAG, "Failed to create WiFi event group");
        return false;
    }

    // Initialize TCP/IP adapter
    esp_err_t ret = esp_netif_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize TCP/IP adapter");
        return false;
    }

    // Create default event loop
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create default event loop");
        return false;
    }

    // Create WiFi station instance
    esp_netif_create_default_wifi_sta();

    // Initialize WiFi
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize WiFi");
        return false;
    }

    // Register event handler
    ret = esp_event_handler_instance_register(WIFI_EVENT, 
                                              ESP_EVENT_ANY_ID,
                                              &WiFiController::eventHandler,
                                              this,
                                              nullptr);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register WiFi event handler");
        return false;
    }

    ret = esp_event_handler_instance_register(IP_EVENT,
                                              IP_EVENT_STA_GOT_IP,
                                              &WiFiController::eventHandler,
                                              this,
                                              nullptr);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register IP event handler");
        return false;
    }

    // Set WiFi mode to station
    ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set WiFi mode to station");
        return false;
    }

    // Start WiFi
    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WiFi");
        return false;
    }

    // Set WiFi TX power to 50% (10 out of 20 dBm max)
    ret = esp_wifi_set_max_tx_power(40); // 40 units = 10dBm (50% of max power which is 20dBm), each unit is 0.25dBm
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set WiFi TX power");
        return false;
    }

    ESP_LOGI(TAG, "WiFi controller initialized successfully with 50%% TX power");
    return true;
}

bool WiFiController::connect(const std::string& ssid, const std::string& password) {
    if (state_ != ConnectionState::DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi already connecting or connected");
        return false;
    }

    ssid_ = ssid;
    password_ = password;

    startConnectProcess(ssid, password);
    return true;
}

void WiFiController::startConnectProcess(const std::string& ssid, const std::string& password) {
    wifi_config_t wifi_config = {};
    strncpy((char*)wifi_config.sta.ssid, ssid.c_str(), sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char*)wifi_config.sta.password, password.c_str(), sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    esp_err_t ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set WiFi configuration");
        state_ = ConnectionState::ERROR;
        return;
    }

    state_ = ConnectionState::CONNECTING;
    ESP_LOGI(TAG, "Connecting to SSID: %s", ssid.c_str());

    ret = esp_wifi_connect();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WiFi connection");
        state_ = ConnectionState::ERROR;
        return;
    }
}

void WiFiController::disconnect() {
    esp_wifi_disconnect();
    state_ = ConnectionState::DISCONNECTED;
    ESP_LOGI(TAG, "Disconnected from WiFi");
}

int8_t WiFiController::getRSSI() const {
    if (state_ != ConnectionState::CONNECTED) {
        return 0;
    }

    wifi_ap_record_t ap_info;
    esp_err_t ret = esp_wifi_sta_get_ap_info(&ap_info);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get AP info");
        return 0;
    }

    return ap_info.rssi;
}

void WiFiController::eventHandler(void* arg, esp_event_base_t event_base,
                                 int32_t event_id, void* event_data) {
    WiFiController* instance = static_cast<WiFiController*>(arg);

    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
                ESP_LOGI(TAG, "WiFi station started");
                break;
            case WIFI_EVENT_STA_CONNECTED:
                ESP_LOGI(TAG, "WiFi station connected");
                break;
            case WIFI_EVENT_STA_DISCONNECTED: {
                instance->state_ = ConnectionState::DISCONNECTED;
                ESP_LOGI(TAG, "WiFi station disconnected");
                
                wifi_event_sta_disconnected_t* disconnected = 
                    static_cast<wifi_event_sta_disconnected_t*>(event_data);
                
                ESP_LOGE(TAG, "Disconnect reason: %d", disconnected->reason);
                
                // Try to reconnect automatically after a delay
                ESP_LOGI(TAG, "Attempting to reconnect...");
                vTaskDelay(pdMS_TO_TICKS(5000));
                instance->startConnectProcess(instance->ssid_, instance->password_);
                break;
            }
            default:
                break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        instance->state_ = ConnectionState::CONNECTED;
        ip_event_got_ip_t* event = static_cast<ip_event_got_ip_t*>(event_data);
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        
        xEventGroupSetBits(instance->wifi_event_group_, WIFI_CONNECTED_BIT);
    }
}