#include <stdio.h>
#include <string>
#include <cstring>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "lwip/netdb.h"
#include "ping/ping_sock.h"
#include <vector>

// --- Configuration ---
#define WIFI_SSID      "WP17"
#define WIFI_PASSWORD  "11111111"

// Global list of target hosts for specific status checks
const std::string FULL_ACCESS_HOST = "google.com";
const std::string RF_SITE_1 = "dzen.ru";
const std::string RF_SITE_2 = "kp40.ru"; // User specified kp40.ru to be checked if dzen.ru is available but google.com is not

static const char *TAG = "PING_APP";

// --- WiFi Connection Logic (based on ESP-IDF example) ---

static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
static int s_retry_num = 0;

static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < 5) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG,"connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void wifi_init_sta() {
    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &instance_got_ip));

    wifi_config_t wifi_config = {};
    strncpy((char*)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid));
    strncpy((char*)wifi_config.sta.password, WIFI_PASSWORD, sizeof(wifi_config.sta.password));
    
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_sta finished.");

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to ap SSID:%s", WIFI_SSID);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s", WIFI_SSID);
    } else {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }
}

// --- Synchronous Ping Logic ---

struct PingContext {
    SemaphoreHandle_t semaphore;
    bool success;
};

static void ping_on_success_cb(esp_ping_handle_t hdl, void *args) {
    auto* ctx = static_cast<PingContext*>(args);
    ctx->success = true; // Mark that at least one packet was received
    // We don't log here to keep it clean, stats are logged in on_end
}

static void ping_on_timeout_cb(esp_ping_handle_t hdl, void *args) {
    // A single packet timed out. The end callback will still be called.
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

    ESP_LOGI(TAG, "Connecting to WiFi...");
    wifi_init_sta();

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