#include "wifi_controller.hpp"
#include <cstring>
#include "esp_log.h"

const char* WiFiController::TAG = "WIFI_CONTROLLER";

// --- Event Bits ---
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

WiFiController::WiFiController() 
    : state_(ConnectionState::DISCONNECTED), 
      wifi_event_group_(nullptr),
      retry_num_(0),
      instance_any_id_(nullptr),
      instance_got_ip_(nullptr) {
}

WiFiController::~WiFiController() {
    if (instance_any_id_) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id_);
    }
    if (instance_got_ip_) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip_);
    }
    if (wifi_event_group_) {
        vEventGroupDelete(wifi_event_group_);
    }
}

bool WiFiController::initialize() {
    esp_err_t ret; // Declare ret here
    wifi_event_group_ = xEventGroupCreate();
    if (!wifi_event_group_) {
        ESP_LOGE(TAG, "Failed to create event group");
        return false;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Register event handlers and pass 'this' context
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &WiFiController::eventHandler, this, &instance_any_id_));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &WiFiController::eventHandler, this, &instance_got_ip_));

    // Start WiFi
    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WiFi");
        return false;
    }

    // Set WiFi TX power to 50% (10 out of 20 dBm max)
    ret = esp_wifi_set_max_tx_power(40); // 40 units = 10dBm (50% of max power)
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set WiFi TX power");
        return false;
    }

    ESP_LOGI(TAG, "WiFi controller initialized successfully.");
    return true;
}

void WiFiController::connect(const std::string& ssid, const std::string& password) {
    if (state_ == ConnectionState::CONNECTING || state_ == ConnectionState::CONNECTED) {
        ESP_LOGW(TAG, "Already connecting or connected.");
        return;
    }
    ssid_ = ssid;
    password_ = password;
    retry_num_ = 0;

    startConnectProcess();
}

void WiFiController::startConnectProcess() {
    wifi_config_t wifi_config = {};
    strncpy((char*)wifi_config.sta.ssid, ssid_.c_str(), sizeof(wifi_config.sta.ssid));
    strncpy((char*)wifi_config.sta.password, password_.c_str(), sizeof(wifi_config.sta.password));
    
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    // Starting the connection process is triggered by WIFI_EVENT_STA_START in the event handler now.
}


WiFiController::ConnectionState WiFiController::getState() const {
    return state_;
}

int8_t WiFiController::getRSSI() const {
    if (state_ != ConnectionState::CONNECTED) {
        return 0;
    }
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        return ap_info.rssi;
    }
    return 0;
}

void WiFiController::eventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    WiFiController* instance = static_cast<WiFiController*>(arg);

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "WiFi station started, connecting...");
        instance->state_ = ConnectionState::CONNECTING;
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        instance->state_ = ConnectionState::DISCONNECTED;
        if (instance->retry_num_ < 5) {
            vTaskDelay(pdMS_TO_TICKS(2000)); // Wait before retrying
            esp_wifi_connect();
            instance->retry_num_++;
            ESP_LOGI(TAG, "Retry to connect to the AP");
        } else {
            xEventGroupSetBits(instance->wifi_event_group_, WIFI_FAIL_BIT);
            ESP_LOGE(TAG, "Failed to connect to the AP after multiple retries");
            instance->state_ = ConnectionState::ERROR;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = static_cast<ip_event_got_ip_t*>(event_data);
        ESP_LOGI(TAG, "Got IP:" IPSTR, IP2STR(&event->ip_info.ip));
        instance->retry_num_ = 0;
        instance->state_ = ConnectionState::CONNECTED;
        xEventGroupSetBits(instance->wifi_event_group_, WIFI_CONNECTED_BIT);
    }
}
