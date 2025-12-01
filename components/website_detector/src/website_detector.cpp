#include "website_detector.hpp"
#include "Pinger.hpp"
#include <cstring>

// Global mutex to serialize ping operations
static SemaphoreHandle_t g_ping_mutex = nullptr;


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
        if (xSemaphoreTake(g_ping_mutex, pdMS_TO_TICKS(15000)) != pdTRUE) { // Increased mutex wait time
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

    // Create a Pinger instance on the heap to reduce stack usage.
    Pinger* pinger = new Pinger(host);
    Pinger::PingStatus status = pinger->ping();
    delete pinger;

    // Translate Pinger status to WebsiteDetector status
    AccessStatus result;
    switch (status) {
        case Pinger::ACCESSIBLE:
            result = AccessStatus::ACCESSIBLE;
            break;
        case Pinger::BLOCKED:
            result = AccessStatus::BLOCKED;
            break;
        case Pinger::ERROR_TIMEOUT:
            // The Pinger class already logs this timeout
            result = AccessStatus::BLOCKED;
            break;
        default:
            // All other Pinger errors (DNS, SESSION, START) are mapped to our ERROR status
            result = AccessStatus::ERROR;
            break;
    }

    // Release the mutex for the next ping operation.
    xSemaphoreGive(g_ping_mutex);

    ESP_LOGI(TAG, "Ping result for %s (%s): %s",
             url.c_str(),
             host.c_str(),
             statusToString(result).c_str());

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