#pragma once

#include <string>
#include "ping/ping_sock.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

class Pinger {
public:
    enum PingStatus {
        ACCESSIBLE,
        BLOCKED,
        ERROR_DNS,
        ERROR_SESSION,
        ERROR_START,
        ERROR_TIMEOUT
    };

    Pinger(const std::string& host, int timeout_ms = 5000, int count = 1);
    ~Pinger();

    PingStatus ping();

private:
    std::string host_;
    int timeout_ms_;
    int count_;
    
    // State for the ping operation, instance-specific
    SemaphoreHandle_t semaphore_;
    uint32_t received_count_;
    esp_ping_handle_t ping_handle_;

    // Static callbacks passed to esp_ping, using 'this' as args
    static void on_ping_success(esp_ping_handle_t hdl, void *args);
    static void on_ping_timeout(esp_ping_handle_t hdl, void *args);
    static void on_ping_end(esp_ping_handle_t hdl, void *args);
};
