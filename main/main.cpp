#include <stdio.h>
#include <string>
#include "esp_heap_caps.h" // For heap memory info

extern "C" {
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "esp_check.h"
#include "nvs_flash.h"
}

#include "wifi_controller.hpp"
#include "website_detector.hpp"

extern "C" void app_main() {
    // Initialize NVS flash storage
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    printf("Initial free heap (internal, 8bit): %zu bytes\n", heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    printf("Initial free heap (total): %zu bytes\n", heap_caps_get_free_size(MALLOC_CAP_DEFAULT));

    // Initialize WiFi controller
    WiFiController wifi_controller;

    if (!wifi_controller.initialize()) {
        printf("Failed to initialize WiFi controller\n");
        return;
    }

    // Try to connect to WiFi
    const std::string ssid = "WP17";
    const std::string password = "11111111";

    printf("Attempting to connect to WiFi: %s\n", ssid.c_str());
    wifi_controller.connect(ssid, password);

    // Create website detector instance
    WebsiteDetector website_detector;

    // Wait for WiFi connection before checking websites
    while (wifi_controller.getState() != WiFiController::ConnectionState::CONNECTED) {
        printf("Waiting for WiFi connection...\n");
        vTaskDelay(2000 / portTICK_PERIOD_MS); // Wait 2 seconds before checking again
    }

    printf("WiFi connected! Starting periodic website accessibility checks...\n");

    // Main loop to perform checks every minute
    while (1) {
        printf("Performing website accessibility check...\n");
        size_t heap_before_check = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        printf("Free heap before checkAllSites (internal, 8bit): %zu bytes\n", heap_before_check);
        printf("Free heap before checkAllSites (total): %zu bytes\n", heap_caps_get_free_size(MALLOC_CAP_DEFAULT));

        // Perform the check
        website_detector.checkAllSites();

        size_t heap_after_check = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        printf("Free heap after checkAllSites (internal, 8bit): %zu bytes (Used in call: %zu)\n", heap_after_check, heap_before_check - heap_after_check);
        printf("Free heap after checkAllSites (total): %zu bytes\n", heap_caps_get_free_size(MALLOC_CAP_DEFAULT));

        // Determine the overall status based on the results
        std::string overall_status = "No Access"; // Default status
        if (website_detector.getFullAccessStatus() == WebsiteDetector::AccessStatus::ACCESSIBLE) {
            overall_status = "Full Access";
        } else if (website_detector.getRfOnlyStatus() == WebsiteDetector::AccessStatus::ACCESSIBLE) {
            overall_status = "RF Only";
        } else if (website_detector.getWhiteListStatus() == WebsiteDetector::AccessStatus::ACCESSIBLE) {
            overall_status = "White List";
        }

        // Print the consolidated status and other info
        printf("\n--- Current Internet Status ---\n");
        printf("Status: %s\n", overall_status.c_str());
        printf("WiFi RSSI: %d dBm\n\n", wifi_controller.getRSSI());

        // Wait for approximately 60 seconds before the next check
        printf("Waiting for 60 seconds before next check...\n");
        vTaskDelay(60000 / portTICK_PERIOD_MS);
    }
}