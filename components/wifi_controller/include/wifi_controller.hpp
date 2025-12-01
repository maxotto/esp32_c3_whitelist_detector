#pragma once

#include <string>
#include "esp_wifi.h"
#include "esp_event.h"
#include "freertos/event_groups.h"

class WiFiController {
public:
    enum class ConnectionState {
        DISCONNECTED,
        CONNECTING,
        CONNECTED,
        ERROR
    };

    WiFiController();
    ~WiFiController();

    bool initialize();
    void connect(const std::string& ssid, const std::string& password);
    ConnectionState getState() const;
    int8_t getRSSI() const;

private:
    static void eventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
    void startConnectProcess();

    static const char* TAG;
    
    // Member variables to hold state
    volatile ConnectionState state_;
    EventGroupHandle_t wifi_event_group_;
    std::string ssid_;
    std::string password_;
    int retry_num_;

    // Event handler instances
    esp_event_handler_instance_t instance_any_id_;
    esp_event_handler_instance_t instance_got_ip_;
};
