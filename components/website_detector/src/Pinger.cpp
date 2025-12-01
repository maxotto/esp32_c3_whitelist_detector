#include "Pinger.hpp"
#include "lwip/netdb.h"

static const char* TAG = "PINGER";

// =========== Static Callback Implementation ===========

void Pinger::on_ping_success(esp_ping_handle_t hdl, void *args) {
    if (args == nullptr) {
        ESP_LOGE("PINGER_CB", "on_ping_success called with null args");
        return;
    }
    ESP_LOGI("PINGER_CB", "on_ping_success");
    Pinger *pinger_instance = static_cast<Pinger*>(args);
    pinger_instance->received_count_++;
}

void Pinger::on_ping_timeout(esp_ping_handle_t hdl, void *args) {
    ESP_LOGW("PINGER_CB", "on_ping_timeout (single packet)");
}

void Pinger::on_ping_end(esp_ping_handle_t hdl, void *args) {
    if (args == nullptr) {
        ESP_LOGE("PINGER_CB", "on_ping_end called with null args");
        return;
    }
    ESP_LOGI("PINGER_CB", "on_ping_end");
    Pinger *pinger_instance = static_cast<Pinger*>(args);
    // Signal completion to the waiting task, do not delete session here.
    xSemaphoreGive(pinger_instance->semaphore_);
}

// =========== Class Method Implementation ===========

Pinger::Pinger(const std::string& host, int timeout_ms, int count)
    : host_(host),
      timeout_ms_(timeout_ms),
      count_(count),
      semaphore_(nullptr),
      received_count_(0),
      ping_handle_(nullptr) {
    semaphore_ = xSemaphoreCreateBinary();
}

Pinger::~Pinger() {
    if (semaphore_ != nullptr) {
        vSemaphoreDelete(semaphore_);
    }
    // The ping() method is now responsible for deleting the handle.
    if (ping_handle_ != nullptr) {
        ESP_LOGE(TAG, "Pinger destroyed with a dangling ping session handle. Deleting now.");
        esp_ping_delete_session(ping_handle_);
    }
}

Pinger::PingStatus Pinger::ping() {
    ESP_LOGI(TAG, "Pinger::ping() started for host: %s", host_.c_str());
    if (semaphore_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create semaphore");
        return ERROR_SESSION;
    }

    // 1. DNS Lookup
    ip_addr_t target_addr;
    struct addrinfo hints;
    struct addrinfo *result_addr = nullptr;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    int ret = getaddrinfo(host_.c_str(), nullptr, &hints, &result_addr);
    if (ret != 0 || result_addr == nullptr) {
        ESP_LOGE(TAG, "DNS lookup failed for host: %s, error: %d", host_.c_str(), ret);
        if (result_addr) freeaddrinfo(result_addr);
        return ERROR_DNS;
    }
    struct in_addr addr4 = ((struct sockaddr_in *)(result_addr->ai_addr))->sin_addr;
    inet_addr_to_ip4addr(ip_2_ip4(&target_addr), &addr4);
    freeaddrinfo(result_addr);

    // 2. Setup Ping Configuration
    esp_ping_config_t ping_config = ESP_PING_DEFAULT_CONFIG();
    ping_config.target_addr = target_addr;
    ping_config.count = this->count_;

    // 3. Setup Callbacks
    esp_ping_callbacks_t cbs = {};
    cbs.on_ping_success = Pinger::on_ping_success;
    cbs.on_ping_timeout = Pinger::on_ping_timeout;
    cbs.on_ping_end = Pinger::on_ping_end;
    cbs.cb_args = this; // Pass instance pointer to callbacks

    // 4. Create and start ping session
    esp_err_t err = esp_ping_new_session(&ping_config, &cbs, &this->ping_handle_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Ping session creation failed for host: %s, error: 0x%x", host_.c_str(), err);
        this->ping_handle_ = nullptr;
        return ERROR_SESSION;
    }

    err = esp_ping_start(this->ping_handle_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Ping start failed for host: %s, error: 0x%x", host_.c_str(), err);
        esp_ping_delete_session(this->ping_handle_);
        this->ping_handle_ = nullptr;
        return ERROR_START;
    }

    // 5. Wait for completion or timeout
    PingStatus result;
    bool timed_out = (xSemaphoreTake(semaphore_, pdMS_TO_TICKS(this->timeout_ms_)) != pdTRUE);

    if (timed_out) {
        ESP_LOGW(TAG, "Pinger::ping() timed out for host: %s", host_.c_str());
        // Stop the session. This triggers on_ping_end, which gives the semaphore.
        esp_ping_stop(ping_handle_);
        // Drain the semaphore that on_ping_end will give.
        xSemaphoreTake(semaphore_, pdMS_TO_TICKS(2000));
        result = ERROR_TIMEOUT;
    } else {
        ESP_LOGI(TAG, "Pinger::ping() completed for host: %s. Received: %d", host_.c_str(), (int)received_count_);
        result = received_count_ > 0 ? ACCESSIBLE : BLOCKED;
    }

    // By now, the session is guaranteed to have stopped (either by finishing or by being stopped).
    // Now we can safely delete it.
    esp_ping_delete_session(ping_handle_);
    ping_handle_ = nullptr;
    
    return result;
}
