#include <stdio.h>
#include <string>
#include <cstring>
#include <vector>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "lwip/netdb.h"
#include "ping/ping_sock.h"
#include "esp_wifi.h" // For wifi_init_sta
#include "led_strip.h" 
#include "led_strip_types.h" // Explicitly include for enums 

// --- Configuration ---
#define WIFI_SSID      "Tuchnevo7Viva"
#define WIFI_PASSWORD  "dtcmvbhnfyrb"
#define LED_STRIP_GPIO 2
#define LED_STRIP_LED_COUNT 4

const std::string FULL_ACCESS_HOST = "google.com";
const std::string RF_SITE_1 = "dzen.ru";
const std::string RF_SITE_2 = "kp40.ru";

static const char *TAG = "PING_APP";

// --- Global Handles ---
static led_strip_handle_t led_strip;

// --- Internet Status Enum ---
enum class InternetStatus {
    FULL_ACCESS,
    RF_SITES_ONLY,
    WHITE_LIST,
    NO_INTERNET,
    UNKNOWN 
};

// --- Forward Declarations ---
void wifi_init_sta();
bool execute_ping(const std::string& host);
std::string statusToString(InternetStatus status);
void initialize_led_strip();
void set_led_strip_color(InternetStatus status);

// --- LED Strip Control ---

struct RgbColor {
    uint32_t r, g, b;
};

static RgbColor s_current_color = {0, 0, 0};
static SemaphoreHandle_t s_color_mutex = NULL;
static TaskHandle_t s_scanner_task_handle = NULL;

void scanner_task(void* param) {
    while(1) {
        for (int i = 0; i < LED_STRIP_LED_COUNT; i++) {
            xSemaphoreTake(s_color_mutex, portMAX_DELAY);
            RgbColor base_color = s_current_color;
            xSemaphoreGive(s_color_mutex);

            // Set all pixels to base color
            for (int j = 0; j < LED_STRIP_LED_COUNT; j++) {
                led_strip_set_pixel(led_strip, j, base_color.r, base_color.g, base_color.b);
            }
            // Dim the currently active pixel
            led_strip_set_pixel(led_strip, i, base_color.r / 4, base_color.g / 4, base_color.b / 4);
            
            led_strip_refresh(led_strip);
            vTaskDelay(pdMS_TO_TICKS(300));
        }
    }
}

void initialize_led_strip() {
    ESP_LOGI(TAG, "Initializing LED strip on GPIO %d", LED_STRIP_GPIO);

    // Initialize all fields explicitly to avoid warnings
    led_strip_config_t strip_config = {
        .strip_gpio_num = LED_STRIP_GPIO,
        .max_leds = LED_STRIP_LED_COUNT,
        .led_model = LED_MODEL_WS2812, // Specify the LED model
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB, // WS2812 typically uses GRB format - corrected name
        .flags = {
            .invert_out = false, // Default value
        },
    };

    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT, // Use default clock source - put first to match declaration order
        .resolution_hz = 10 * 1000 * 1000, // 10MHz
        .mem_block_symbols = 64, // Default safe value
        .flags = {
            .with_dma = false, // Default value
        },
    };

    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));
    // Clear the strip upon initialization
    led_strip_clear(led_strip);
    ESP_LOGI(TAG, "LED strip initialized");
}

void set_led_strip_color(InternetStatus status) {
    RgbColor new_color = {0, 0, 0};

    switch (status) {
        case InternetStatus::FULL_ACCESS:
            new_color = {0, 128, 0}; // Green
            break;
        case InternetStatus::RF_SITES_ONLY:
            new_color = {128, 128, 0}; // Yellow
            break;
        case InternetStatus::WHITE_LIST:
            new_color = {255, 100, 0}; // Orange
            break;
        case InternetStatus::NO_INTERNET:
            new_color = {128, 0, 0}; // Red
            break;
        case InternetStatus::UNKNOWN:
        default:
            new_color = {1, 1, 1}; // Off
            break;
    }

    ESP_LOGI(TAG, "Setting LED color for status %s -> R:%d G:%d B:%d", statusToString(status).c_str(), (int)new_color.r, (int)new_color.g, (int)new_color.b);

    xSemaphoreTake(s_color_mutex, portMAX_DELAY);
    s_current_color = new_color;
    xSemaphoreGive(s_color_mutex);

    for (int i = 0; i < LED_STRIP_LED_COUNT; i++) {
        ESP_ERROR_CHECK(led_strip_set_pixel(led_strip, i, new_color.r, new_color.g, new_color.b));
    }
    ESP_ERROR_CHECK(led_strip_refresh(led_strip));
}


// --- WiFi Connection Logic ---
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0

static TaskHandle_t s_blink_task_handle = NULL;

void wifi_connecting_blink_task(void *pvParameters) {
    bool led_on = false;
    while (1) {
        if (led_on) {
            // Turn LED off
            led_strip_set_pixel(led_strip, 0, 0, 0, 0);
        } else {
            // Turn LED blue
            led_strip_set_pixel(led_strip, 0, 0, 0, 128);
        }
        led_strip_refresh(led_strip);
        led_on = !led_on;
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) { 
        esp_wifi_connect(); 
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "Disconnected. Restarting device due to WiFi loss...");
        if (s_blink_task_handle != NULL) {
            vTaskDelete(s_blink_task_handle);
            s_blink_task_handle = NULL;
        }
        // Set all LEDs to red to indicate restart
        for (int i = 0; i < LED_STRIP_LED_COUNT; i++) {
            led_strip_set_pixel(led_strip, i, 128, 0, 0); // Red
        }
        led_strip_refresh(led_strip);
        vTaskDelay(pdMS_TO_TICKS(2000)); // Delay for 2 seconds
        esp_restart();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        if (s_blink_task_handle != NULL) { // Stop blinking
            vTaskDelete(s_blink_task_handle);
            s_blink_task_handle = NULL;
            led_strip_clear(led_strip); // Clean up LED state
            led_strip_refresh(led_strip);
        }
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

    esp_event_handler_instance_t instance_any_id, instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &instance_got_ip));
    wifi_config_t wifi_config = {};
    strncpy((char*)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid));
    strncpy((char*)wifi_config.sta.password, WIFI_PASSWORD, sizeof(wifi_config.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Set WiFi TX power to 60% (48 units out of 80 max) after WiFi has started
    ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(48)); 

    ESP_LOGI(TAG, "wifi_init_sta finished.");
    ESP_LOGI(TAG, "Waiting for WiFi connection...");
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE, portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) { 
        ESP_LOGI(TAG, "Connected to ap SSID:%s", WIFI_SSID); 
    } else { 
        ESP_LOGE(TAG, "UNEXPECTED EVENT while waiting for connection."); 
    }
}


// --- Synchronous Ping Logic ---
struct PingContext { SemaphoreHandle_t semaphore; bool success; };
static void ping_on_success_cb(esp_ping_handle_t hdl, void *args) { auto* ctx = static_cast<PingContext*>(args); ctx->success = true; }
static void ping_on_timeout_cb(esp_ping_handle_t hdl, void *args) {}
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
    ESP_LOGI(TAG, "%" PRIu32 " pkts transmitted, %" PRIu32 " received, %" PRIu32 "%% loss, time %" PRIu32 "ms", transmitted, received, loss, total_time_ms);
    esp_ping_delete_session(hdl);
    xSemaphoreGive(ctx->semaphore);
}
bool execute_ping(const std::string& host) {
    ESP_LOGI(TAG, "Pinging host: %s", host.c_str());
    PingContext ctx;
    ctx.semaphore = xSemaphoreCreateBinary();
    ctx.success = false;
    if (ctx.semaphore == NULL) { ESP_LOGE(TAG, "Failed to create semaphore"); return false; }
    esp_ping_config_t config = ESP_PING_DEFAULT_CONFIG();
    ip_addr_t target_addr;
    memset(&target_addr, 0, sizeof(target_addr));
    struct addrinfo hint;
    memset(&hint, 0, sizeof(hint));
    struct addrinfo *res = NULL;
    int err = getaddrinfo(host.c_str(), NULL, &hint, &res);
    if(err != 0 || res == NULL) { ESP_LOGE(TAG, "DNS lookup failed for host %s", host.c_str()); vSemaphoreDelete(ctx.semaphore); if (res) freeaddrinfo(res); return false; }
    struct in_addr addr4 = ((struct sockaddr_in *)(res->ai_addr))->sin_addr;
    inet_addr_to_ip4addr(ip_2_ip4(&target_addr), &addr4);
    freeaddrinfo(res);
    config.target_addr = target_addr;
    config.count = 5;
    config.timeout_ms = 1000;
    esp_ping_callbacks_t cbs = { .cb_args = &ctx, .on_ping_success = ping_on_success_cb, .on_ping_timeout = ping_on_timeout_cb, .on_ping_end = ping_on_end_cb };
    esp_ping_handle_t ping;
    esp_err_t ret = esp_ping_new_session(&config, &cbs, &ping);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "Failed to create ping session: %s", esp_err_to_name(ret)); vSemaphoreDelete(ctx.semaphore); return false; }
    ret = esp_ping_start(ping);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "Failed to start ping session: %s", esp_err_to_name(ret)); esp_ping_delete_session(ping); vSemaphoreDelete(ctx.semaphore); return false; }
    if (xSemaphoreTake(ctx.semaphore, pdMS_TO_TICKS(config.timeout_ms * config.count + 2000)) != pdTRUE) {
        ESP_LOGE(TAG, "Ping for %s timed out globally.", host.c_str());
        esp_ping_stop(ping);
        xSemaphoreTake(ctx.semaphore, pdMS_TO_TICKS(1000));
    }
    vSemaphoreDelete(ctx.semaphore);
    return ctx.success;
}

// --- Status to String Helper ---
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

// --- Main Application Logic ---
extern "C" void app_main() {
    // --- Initialization ---
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    initialize_led_strip();
    s_color_mutex = xSemaphoreCreateMutex();
    
    // Start blinking before initiating WiFi connection
    if (s_blink_task_handle == NULL) {
        xTaskCreate(wifi_connecting_blink_task, "wifi_blink", 2048, NULL, 5, &s_blink_task_handle);
    }
    
    wifi_init_sta();

    // The wifi_event_handler will stop the blink task upon successful connection
    set_led_strip_color(InternetStatus::UNKNOWN); // Initial color state

    // --- Main Loop ---
    while(1) {
        ESP_LOGI(TAG, "--- Starting new round of status checks ---");
        
        // Start scanner animation
        xTaskCreate(scanner_task, "scanner", 2048, NULL, 5, &s_scanner_task_handle);

        // --- Diagnostic Ping ---
        bool router_ok = execute_ping("192.168.1.1");
        ESP_LOGI(TAG, "Diagnostic: Ping to router (192.168.1.1) was %s", router_ok ? "SUCCESSFUL" : "FAILED");
        // --- End of Diagnostic Ping ---

        bool google_ok = execute_ping(FULL_ACCESS_HOST);
        bool dzen_ok = execute_ping(RF_SITE_1);
        bool kp40_ok = execute_ping(RF_SITE_2);
        
        // Stop scanner animation
        if(s_scanner_task_handle != NULL) {
            vTaskDelete(s_scanner_task_handle);
            s_scanner_task_handle = NULL;
        }

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
        set_led_strip_color(current_status);
        
        ESP_LOGI(TAG, "Waiting 60 seconds for next check...");
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}