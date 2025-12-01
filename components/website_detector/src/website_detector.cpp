#include "website_detector.hpp"
#include <cstring>

// Global mutex to serialize ping operations
static SemaphoreHandle_t g_ping_mutex = nullptr;

// A struct to hold the state for a single ping operation.
struct PingState {
    SemaphoreHandle_t semaphore;
    uint32_t received_count;
};

// Static callback handlers for ping operations
static void on_ping_success(esp_ping_handle_t hdl, void *args) {
    PingState *state = static_cast<PingState*>(args);
    state->received_count++;
}

static void on_ping_timeout(esp_ping_handle_t hdl, void *args) {
    // This is a timeout for a single packet, not the whole session
}

static void on_ping_end(esp_ping_handle_t hdl, void *args) {
    PingState *state = static_cast<PingState*>(args);
    
    // As per ESP-IDF example, delete session in the callback
    esp_ping_delete_session(hdl);

    // Signal completion to the waiting task
    xSemaphoreGive(state->semaphore);
}

// Initialize the global mutex at startup
static void initialize_ping_mutex() {
    if (g_ping_mutex == nullptr) {
        g_ping_mutex = xSemaphoreCreateMutex();
    }
}

WebsiteDetector::WebsiteDetector() {
    // Initialize with default sites
    initialize_ping_mutex();
    setDefaultSites();
}

WebsiteDetector::~WebsiteDetector() {
    // Cleanup if needed
    if (g_ping_mutex != nullptr) {
        vSemaphoreDelete(g_ping_mutex);
        g_ping_mutex = nullptr;
    }
}

void WebsiteDetector::addSiteToFullAccessList(const std::string& url, const std::string& name) {
    std::string site_name = name.empty() ? "Full Access Site" : name;
    full_access_list_.emplace_back(site_name, url);
}

void WebsiteDetector::addSiteToRfOnlyList(const std::string& url, const std::string& name) {
    std::string site_name = name.empty() ? "RF Only Site" : name;
    rf_only_list_.emplace_back(site_name, url);
}

void WebsiteDetector::addSiteToWhiteList(const std::string& url, const std::string& name) {
    std::string site_name = name.empty() ? "White List Site" : name;
    white_list_.emplace_back(site_name, url);
}

void WebsiteDetector::setDefaultSites() {
    // Clear existing lists
    full_access_list_.clear();
    rf_only_list_.clear();
    white_list_.clear();

    // Add default sites in different order
    addSiteToWhiteList("https://dzen.ru", "Dzen");
    addSiteToRfOnlyList("https://kp40.ru", "KP40");
    addSiteToFullAccessList("https://google.com", "Google");
}

WebsiteDetector::AccessStatus WebsiteDetector::checkWebsite(const std::string& url) {
    // Acquire the global ping mutex to ensure only one ping session at a time
    if (g_ping_mutex != nullptr) {
        if (xSemaphoreTake(g_ping_mutex, pdMS_TO_TICKS(10000)) != pdTRUE) {
            ESP_LOGE(TAG, "Failed to acquire ping mutex");
            return AccessStatus::ERROR;
        }
    } else {
        ESP_LOGE(TAG, "Ping mutex not initialized");
        return AccessStatus::ERROR;
    }

    // Extract hostname from URL
    std::string host;
    size_t start_pos = 0;
    if (url.substr(0, 8) == "https://") {
        start_pos = 8;
    } else if (url.substr(0, 7) == "http://") {
        start_pos = 7;
    } else {
        host = url;
    }

    if (start_pos > 0) {
        size_t end_pos = url.find('/', start_pos);
        if (end_pos != std::string::npos) {
            host = url.substr(start_pos, end_pos - start_pos);
        } else {
            host = url.substr(start_pos);
        }
    }

    ESP_LOGI(TAG, "Attempting to ping host: %s", host.c_str());

    // DNS Lookup
    ip_addr_t target_addr;
    struct addrinfo hints;
    struct addrinfo *result_addr = nullptr;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    int ret = getaddrinfo(host.c_str(), nullptr, &hints, &result_addr);
    if (ret != 0 || result_addr == nullptr) {
        ESP_LOGE(TAG, "DNS lookup failed for host: %s, error: %d", host.c_str(), ret);
        if (result_addr) freeaddrinfo(result_addr);
        xSemaphoreGive(g_ping_mutex);
        return AccessStatus::ERROR;
    }
    struct in_addr addr4 = ((struct sockaddr_in *)(result_addr->ai_addr))->sin_addr;
    inet_addr_to_ip4addr(ip_2_ip4(&target_addr), &addr4);
    freeaddrinfo(result_addr);

    // Setup Ping Configuration
    esp_ping_config_t ping_config = ESP_PING_DEFAULT_CONFIG();
    ping_config.target_addr = target_addr;
    ping_config.count = 1; // Use only 1 ping instead of 3 to reduce resource usage

    // Setup state and synchronization objects
    PingState ping_state;
    ping_state.received_count = 0;
    ping_state.semaphore = xSemaphoreCreateBinary();
    if (ping_state.semaphore == nullptr) {
        ESP_LOGE(TAG, "Failed to create semaphore");
        xSemaphoreGive(g_ping_mutex);
        return AccessStatus::ERROR;
    }

    // Setup callbacks
    esp_ping_callbacks_t cbs = {};
    cbs.on_ping_success = on_ping_success;
    cbs.on_ping_timeout = on_ping_timeout;
    cbs.on_ping_end = on_ping_end;
    cbs.cb_args = &ping_state;

    // Create and start ping session
    esp_ping_handle_t ping_handle;
    esp_err_t err = esp_ping_new_session(&ping_config, &cbs, &ping_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Ping session creation failed for host: %s, error: 0x%x", host.c_str(), err);
        vSemaphoreDelete(ping_state.semaphore);
        xSemaphoreGive(g_ping_mutex);
        return AccessStatus::BLOCKED;
    }

    err = esp_ping_start(ping_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Ping start failed for host: %s, error: 0x%x", host.c_str(), err);
        esp_ping_delete_session(ping_handle);
        vSemaphoreDelete(ping_state.semaphore);
        xSemaphoreGive(g_ping_mutex);
        return AccessStatus::BLOCKED;
    }

    // Wait for the ping to complete. The on_ping_end callback will delete the
    // session and then give the semaphore.
    AccessStatus result;
    if (xSemaphoreTake(ping_state.semaphore, pdMS_TO_TICKS(5000)) == pdTRUE) {
        // Ping completed and session was deleted in callback. Check results.
        if (ping_state.received_count > 0) {
            result = AccessStatus::ACCESSIBLE;
        } else {
            result = AccessStatus::BLOCKED;
        }
    } else {
        // Ping timed out. The on_ping_end callback was NOT called.
        ESP_LOGW(TAG, "Ping session timed out for host: %s", host.c_str());
        result = AccessStatus::BLOCKED;

        // The session is still active. Stop it.
        // This will trigger on_ping_end, which deletes the session and gives the semaphore.
        esp_ping_stop(ping_handle);

        // Wait for the on_ping_end callback to run and signal us.
        xSemaphoreTake(ping_state.semaphore, pdMS_TO_TICKS(2000));
    }

    // The session is now deleted. Just clean up the semaphore.
    vSemaphoreDelete(ping_state.semaphore);

    // Release the mutex for the next ping operation.
    xSemaphoreGive(g_ping_mutex);

    ESP_LOGI(TAG, "Ping result for %s: %s (received %u of 1 packets)",
             host.c_str(),
             result == AccessStatus::ACCESSIBLE ? "ACCESSIBLE" : "BLOCKED",
             static_cast<unsigned int>(ping_state.received_count));

    return result;
}

void WebsiteDetector::checkFullAccessList() {
    for (auto& site : full_access_list_) {
        site.status = checkWebsite(site.url);
        ESP_LOGI(TAG, "Full Access Site '%s' (%s) status: %s",
                 site.name.c_str(), site.url.c_str(),
                 statusToString(site.status).c_str());
    }
}

void WebsiteDetector::checkRfOnlyList() {
    for (auto& site : rf_only_list_) {
        site.status = checkWebsite(site.url);
        ESP_LOGI(TAG, "RF Only Site '%s' (%s) status: %s",
                 site.name.c_str(), site.url.c_str(),
                 statusToString(site.status).c_str());
    }
}

void WebsiteDetector::checkWhiteList() {
    for (auto& site : white_list_) {
        site.status = checkWebsite(site.url);
        ESP_LOGI(TAG, "White List Site '%s' (%s) status: %s",
                 site.name.c_str(), site.url.c_str(),
                 statusToString(site.status).c_str());
    }
}

void WebsiteDetector::checkAllSites() {
    ESP_LOGI(TAG, "Starting website accessibility check for all lists...");

    checkWhiteList();
    checkRfOnlyList();
    checkFullAccessList();

    ESP_LOGI(TAG, "Website accessibility check completed.");
}

WebsiteDetector::AccessStatus WebsiteDetector::getFullAccessStatus() const {
    if (full_access_list_.empty()) {
        return AccessStatus::UNKNOWN;
    }

    // Return the "worst" status among all sites in the list
    AccessStatus worst_status = AccessStatus::ACCESSIBLE;
    for (const auto& site : full_access_list_) {
        if (site.status == AccessStatus::ERROR) {
            return AccessStatus::ERROR;
        } else if (site.status == AccessStatus::BLOCKED && worst_status != AccessStatus::ERROR) {
            worst_status = AccessStatus::BLOCKED;
        } else if (site.status == AccessStatus::UNKNOWN &&
                   worst_status != AccessStatus::ERROR &&
                   worst_status != AccessStatus::BLOCKED) {
            worst_status = AccessStatus::UNKNOWN;
        }
    }
    return worst_status;
}

WebsiteDetector::AccessStatus WebsiteDetector::getRfOnlyStatus() const {
    if (rf_only_list_.empty()) {
        return AccessStatus::UNKNOWN;
    }

    AccessStatus worst_status = AccessStatus::ACCESSIBLE;
    for (const auto& site : rf_only_list_) {
        if (site.status == AccessStatus::ERROR) {
            return AccessStatus::ERROR;
        } else if (site.status == AccessStatus::BLOCKED && worst_status != AccessStatus::ERROR) {
            worst_status = AccessStatus::BLOCKED;
        } else if (site.status == AccessStatus::UNKNOWN &&
                   worst_status != AccessStatus::ERROR &&
                   worst_status != AccessStatus::BLOCKED) {
            worst_status = AccessStatus::UNKNOWN;
        }
    }
    return worst_status;
}

WebsiteDetector::AccessStatus WebsiteDetector::getWhiteListStatus() const {
    if (white_list_.empty()) {
        return AccessStatus::UNKNOWN;
    }

    AccessStatus worst_status = AccessStatus::ACCESSIBLE;
    for (const auto& site : white_list_) {
        if (site.status == AccessStatus::ERROR) {
            return AccessStatus::ERROR;
        } else if (site.status == AccessStatus::BLOCKED && worst_status != AccessStatus::ERROR) {
            worst_status = AccessStatus::BLOCKED;
        } else if (site.status == AccessStatus::UNKNOWN &&
                   worst_status != AccessStatus::ERROR &&
                   worst_status != AccessStatus::BLOCKED) {
            worst_status = AccessStatus::UNKNOWN;
        }
    }
    return worst_status;
}

std::string WebsiteDetector::statusToString(AccessStatus status) {
    switch (status) {
        case AccessStatus::ACCESSIBLE: return "ACCESSIBLE";
        case AccessStatus::BLOCKED: return "BLOCKED";
        case AccessStatus::ERROR: return "ERROR";
        case AccessStatus::UNKNOWN: return "UNKNOWN";
        default: return "UNKNOWN";
    }
}