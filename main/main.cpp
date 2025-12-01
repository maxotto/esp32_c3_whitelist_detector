#include <stdio.h>
#include <string>
#include <cstring>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "lwip/netdb.h"
#include "ping/ping_sock.h"
#include <vector>

#include "wifi_controller.hpp" // Include the WiFi controller class

// --- Configuration ---
#define WIFI_SSID      "Tuchnevo7"
#define WIFI_PASSWORD  "dtcmvbhnfyrb"

// Global list of target hosts for specific status checks
const std::string FULL_ACCESS_HOST = "google.com";
const std::string RF_SITE_1 = "dzen.ru";
const std::string RF_SITE_2 = "kp40.ru";

static const char *TAG = "PING_APP";


// --- Synchronous Ping Logic ---

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
    PingContext* ctx = static_cast<PingContext*>(args);

    ip_addr_t target_addr;
    uint32_t transmitted, received, total_time_ms, loss;
    esp_ping_get_profile(hdl, ESP_PING_PROF_REQUEST, &transmitted, sizeof(transmitted));
    esp_ping_get_profile(hdl, ESP_PING_PROF_REPLY, &received, sizeof(received));
    esp_ping_get_profile(hdl, ESP_PING_PROF_IPADDR, &target_addr, sizeof(target_addr));
    esp_ping_get_profile(hdl, ESP_PING_PROF_DURATION, &total_time_ms, sizeof(total_time_ms));
    loss = transmitted > 0 ? (uint32_t)((1 - ((float)received) / transmitted) * 100) : 0;

    ESP_LOGI(TAG, "--- %s ping statistics ---", ipaddr_ntoa(&target_addr));
    ESP_LOGI(TAG, "%" PRIu32 " pkts transmitted, %" PRIu32 " received, %" PRIu32 "%% loss, time %" PRIu32 "ms",
           transmitted, received, loss, total_time_ms);

    esp_ping_delete_session(hdl);
    xSemaphoreGive(ctx->semaphore);
}

static bool execute_ping(const std::string& host) {
    ESP_LOGI(TAG, "Pinging host: %s", host.c_str());

    PingContext ctx;
    ctx.semaphore = xSemaphoreCreateBinary();
    ctx.success = false;
    if (ctx.semaphore == NULL) {
        ESP_LOGE(TAG, "Failed to create semaphore");
        return false;
    }

    esp_ping_config_t config = ESP_PING_DEFAULT_CONFIG();
    
    ip_addr_t target_addr;
    memset(&target_addr, 0, sizeof(target_addr));
    struct addrinfo hint;
    memset(&hint, 0, sizeof(hint));
    struct addrinfo *res = NULL;
    
    int err = getaddrinfo(host.c_str(), NULL, &hint, &res);
    if(err != 0 || res == NULL) {
        ESP_LOGE(TAG, "DNS lookup failed for host %s", host.c_str());
        vSemaphoreDelete(ctx.semaphore);
        if (res) freeaddrinfo(res);
        return false;
    }
    struct in_addr addr4 = ((struct sockaddr_in *)(res->ai_addr))->sin_addr;
    inet_addr_to_ip4addr(ip_2_ip4(&target_addr), &addr4);
    freeaddrinfo(res);

    config.target_addr = target_addr;
    config.count = 3; // Ping 3 times
    config.timeout_ms = 1000;

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

    // Wait for the ping_on_end_sync callback
    if (xSemaphoreTake(ctx.semaphore, pdMS_TO_TICKS(config.timeout_ms * config.count + 2000)) != pdTRUE) {
        ESP_LOGE(TAG, "Ping for %s timed out globally.", host.c_str());
        // Session might still be running, so we need to stop it.
        // This will trigger the on_end callback which will give the semaphore
        esp_ping_stop(ping);
        // Drain the semaphore
        xSemaphoreTake(ctx.semaphore, pdMS_TO_TICKS(1000));
    }
    
    vSemaphoreDelete(ctx.semaphore);
    return ctx.success;
}


// --- Main ---

// Define InternetStatus enum
enum class InternetStatus {
    FULL_ACCESS,
    RF_SITES_ONLY,
    WHITE_LIST,
    NO_INTERNET,
    UNKNOWN // For initialization or error states
};

// Helper function to convert status to string
std::string statusToString(InternetStatus status) {
    switch (status) {
        case InternetStatus::FULL_ACCESS:   return "FULL_ACCESS";
        case InternetStatus::RF_SITES_ONLY: return "RF_SITES_ONLY";
        case InternetStatus::WHITE_LIST:    return "WHITE_LIST";
        case InternetStatus::NO_INTERNET:   return "NO_INTERNET";
        case InternetStatus::UNKNOWN:       return "UNKNOWN";
        default:                            return "INVALID";
    }
}


extern "C" void app_main() {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize and connect WiFi using the controller component
    WiFiController wifi_controller;
    if (!wifi_controller.initialize()) {
        ESP_LOGE(TAG, "Failed to initialize WiFi controller");
        return;
    }
    wifi_controller.connect(WIFI_SSID, WIFI_PASSWORD);


    // Wait for WiFi connection before checking websites
    while (wifi_controller.getState() != WiFiController::ConnectionState::CONNECTED) {
        ESP_LOGI(TAG, "Waiting for WiFi connection...");
        vTaskDelay(2000 / portTICK_PERIOD_MS);
    }
    ESP_LOGI(TAG, "WiFi connected!");

    while(1) {
        ESP_LOGI(TAG, "--- Starting new round of status checks ---");

        // --- Step 1: Ping all sites sequentially ---
        bool google_ok = execute_ping(FULL_ACCESS_HOST);
        vTaskDelay(pdMS_TO_TICKS(1000)); 

        bool dzen_ok = execute_ping(RF_SITE_1);
        vTaskDelay(pdMS_TO_TICKS(1000));

        bool kp40_ok = execute_ping(RF_SITE_2);
        vTaskDelay(pdMS_TO_TICKS(1000));

        // --- Step 2: Determine status based on all results ---
        InternetStatus current_status;
        if (google_ok) {
            current_status = InternetStatus::FULL_ACCESS;
        } else if (dzen_ok && kp40_ok) {
            current_status = InternetStatus::RF_SITES_ONLY;
        } else if (dzen_ok && !kp40_ok) {
            current_status = InternetStatus::WHITE_LIST;
        } else {
            current_status = InternetStatus::NO_INTERNET;
        }
        
        ESP_LOGI(TAG, "--- Current Internet Status: %s ---", statusToString(current_status).c_str());
        
        ESP_LOGI(TAG, "Waiting 60 seconds for next check...");
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}