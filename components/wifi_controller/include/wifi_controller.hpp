#ifndef WIFI_CONTROLLER_HPP
#define WIFI_CONTROLLER_HPP

#include <string>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"

class WiFiController {
public:
    static constexpr const char* TAG = "WiFiController";
    
    enum class ConnectionState {
        DISCONNECTED,
        CONNECTING,
        CONNECTED,
        ERROR
    };

    WiFiController();
    ~WiFiController();

    bool initialize();
    bool connect(const std::string& ssid, const std::string& password);
    void disconnect();
    ConnectionState getState() const { return state_; }
    bool isConnected() const { return state_ == ConnectionState::CONNECTED; }
    std::string getSSID() const { return ssid_; }
    int8_t getRSSI() const;
    
private:
    static void eventHandler(void* arg, esp_event_base_t event_base,
                            int32_t event_id, void* event_data);
    
    void startConnectProcess(const std::string& ssid, const std::string& password);
    
    ConnectionState state_;
    std::string ssid_;
    std::string password_;
    EventGroupHandle_t wifi_event_group_;
    static constexpr int WIFI_CONNECTED_BIT = BIT0;
    static constexpr int WIFI_DISCONNECTED_BIT = BIT1;
};

#endif // WIFI_CONTROLLER_HPP