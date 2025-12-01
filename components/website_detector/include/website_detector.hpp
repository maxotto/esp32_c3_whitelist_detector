#ifndef WEBSITE_DETECTOR_HPP
#define WEBSITE_DETECTOR_HPP

#include <string>
#include <vector>
#include <map>
#include <chrono>
#include <netdb.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "ping/ping_sock.h"

class WebsiteDetector {
public:
    static constexpr const char* TAG = "WebsiteDetector";

    enum class AccessStatus {
        UNKNOWN,
        ACCESSIBLE,
        BLOCKED,
        ERROR
    };

    struct SiteInfo {
        std::string name;
        std::string url;
        AccessStatus status;
        
        SiteInfo(const std::string& n, const std::string& u) 
            : name(n), url(u), status(AccessStatus::UNKNOWN) {}
    };

    WebsiteDetector();
    ~WebsiteDetector();

    // Add sites to specific lists
    void addSiteToFullAccessList(const std::string& url, const std::string& name = "");
    void addSiteToRfOnlyList(const std::string& url, const std::string& name = "");
    void addSiteToWhiteList(const std::string& url, const std::string& name = "");

    // Set default sites as per requirements
    void setDefaultSites();

    // Check accessibility of all sites in all lists
    void checkAllSites();

    // Check accessibility of sites in specific list
    void checkFullAccessList();
    void checkRfOnlyList();
    void checkWhiteList();

    // Get results
    AccessStatus getFullAccessStatus() const;
    AccessStatus getRfOnlyStatus() const;
    AccessStatus getWhiteListStatus() const;

    // Get detailed results for a list
    const std::vector<SiteInfo>& getFullAccessList() const { return full_access_list_; }
    const std::vector<SiteInfo>& getRfOnlyList() const { return rf_only_list_; }
    const std::vector<SiteInfo>& getWhiteList() const { return white_list_; }

    // Helper function to convert status to string
    static std::string statusToString(AccessStatus status);

private:
    std::vector<SiteInfo> full_access_list_;
    std::vector<SiteInfo> rf_only_list_;
    std::vector<SiteInfo> white_list_;
    
    // Internal checking methods
    AccessStatus checkWebsite(const std::string& url);
};

#endif // WEBSITE_DETECTOR_HPP