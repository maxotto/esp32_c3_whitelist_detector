#include "website_detector.hpp"
#include <cstring>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "lwip/netdb.h"
#include "ping/ping_sock.h"

static const char* TAG = "WEBSITE_DETECTOR";

// --- Synchronous Ping Logic (now as a private class method) ---

struct PingContext {
    SemaphoreHandle_t semaphore;
    bool success;
};

static void ping_on_success_cb(esp_ping_handle_t hdl, void *args) {
    auto* ctx = static_cast<PingContext*>(args);
    ctx->success = true;
}

static void ping_on_timeout_cb(esp_ping_handle_t hdl, void *args) {
    // A single packet timed out.
}

static void ping_on_end_cb(esp_ping_handle_t hdl, void *args) {
    auto* ctx = static_cast<PingContext*>(args);
    esp_ping_delete_session(hdl);
    xSemaphoreGive(ctx->semaphore);
}

bool WebsiteDetector::pingHost(const std::string& host) {
    ESP_LOGI(TAG, "Pinging host: %s", host.c_str());

    PingContext ctx;
    ctx.semaphore = xSemaphoreCreateBinary();
    ctx.success = false;
    if (ctx.semaphore == NULL) {
        ESP_LOGE(TAG, "Failed to create semaphore");
        return false;
    }

    esp_ping_config_t config = ESP_PING_DEFAULT_CONFIG();
    config.count = 3;
    config.timeout_ms = 1000;
    
    ip_addr_t target_addr;
    memset(&target_addr, 0, sizeof(target_addr));
    struct addrinfo hint;
    memset(&hint, 0, sizeof(hint));
    struct addrinfo *res = NULL;
    
    int err = getaddrinfo(host.c_str(), NULL, &hint, &res);
    if(err != 0 || res == NULL) {
        ESP_LOGE(TAG, "DNS lookup failed for host %s", host.c_str());
        vSemaphoreDelete(ctx.semaphore);
        freeaddrinfo(res);
        return false;
    }
    struct in_addr addr4 = ((struct sockaddr_in *)(res->ai_addr))->sin_addr;
    inet_addr_to_ip4addr(ip_2_ip4(&target_addr), &addr4);
    freeaddrinfo(res);

    config.target_addr = target_addr;

    esp_ping_callbacks_t cbs = {
        .cb_args = &ctx,
        .on_ping_success = ping_on_success_cb,
        .on_ping_timeout = ping_on_timeout_cb,
        .on_ping_end = ping_on_end_cb
    };

    esp_ping_handle_t ping;
    esp_err_t ret = esp_ping_new_session(&config, &cbs, &ping);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create ping session: %s", esp_err_to_name(ret));
        vSemaphoreDelete(ctx.semaphore);
        return false;
    }
    
    ret = esp_ping_start(ping);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start ping session: %s", esp_err_to_name(ret));
        esp_ping_delete_session(ping);
        vSemaphoreDelete(ctx.semaphore);
        return false;
    }

    if (xSemaphoreTake(ctx.semaphore, pdMS_TO_TICKS(config.timeout_ms * config.count + 2000)) != pdTRUE) {
        ESP_LOGE(TAG, "Ping for %s timed out globally.", host.c_str());
        esp_ping_stop(ping);
        xSemaphoreTake(ctx.semaphore, pdMS_TO_TICKS(1000)); // Drain semaphore
    }
    
    vSemaphoreDelete(ctx.semaphore);
    ESP_LOGI(TAG, "Host %s is %s", host.c_str(), ctx.success ? "ACCESSIBLE" : "BLOCKED");
    return ctx.success;
}


// --- WebsiteDetector Class Implementation ---

WebsiteDetector::WebsiteDetector() {
    // As per user's original request
    full_access_site_ = "google.com";
    rf_site_1_ = "dzen.ru";
    rf_site_2_ = "kp40.ru";
}

WebsiteDetector::~WebsiteDetector() {
}

WebsiteDetector::InternetStatus WebsiteDetector::checkStatus() {
    ESP_LOGI(TAG, "--- Starting new round of status checks ---");
    
    bool google_ok = pingHost(full_access_site_);
    vTaskDelay(pdMS_TO_TICKS(1000)); // Delay between hosts

    if (google_ok) {
        return InternetStatus::FULL_ACCESS;
    }

    // Google failed, check the other two
    bool dzen_ok = pingHost(rf_site_1_);
    vTaskDelay(pdMS_TO_TICKS(1000));

    bool kp40_ok = pingHost(rf_site_2_);

    if (dzen_ok && kp40_ok) {
        return InternetStatus::RF_SITES_ONLY;
    }

    if (dzen_ok && !kp40_ok) {
        return InternetStatus::WHITE_LIST;
    }

    return InternetStatus::NO_INTERNET;
}

std::string WebsiteDetector::statusToString(InternetStatus status) {
    switch(status) {
        case InternetStatus::FULL_ACCESS:   return "FULL_ACCESS";
        case InternetStatus::RF_SITES_ONLY: return "RF_SITES_ONLY";
        case InternetStatus::WHITE_LIST:    return "WHITE_LIST";
        case InternetStatus::NO_INTERNET:   return "NO_INTERNET";
        default:                            return "UNKNOWN";
    }
}
