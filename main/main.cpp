#include <stdio.h>
#include <string>

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

    // Initialize WiFi controller
    WiFiController wifi_controller;

    if (!wifi_controller.initialize()) {
        printf("Failed to initialize WiFi controller\n");
        return;
    }

    // Try to connect to WiFi
    const std::string ssid = "Tuchnevo7";
    const std::string password = "dtcmvbhnfyrb";

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

        // Perform the check
        website_detector.checkAllSites();

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