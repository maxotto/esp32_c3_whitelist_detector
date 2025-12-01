#pragma once

#include <string>
#include <vector>

class WebsiteDetector {
public:
    enum class InternetStatus {
        FULL_ACCESS,
        RF_SITES_ONLY,
        WHITE_LIST,
        NO_INTERNET
    };

    WebsiteDetector();
    ~WebsiteDetector();

    InternetStatus checkStatus();
    std::string statusToString(InternetStatus status);

private:
    // This private method will contain the proven synchronous ping logic.
    bool pingHost(const std::string& host);

    std::string full_access_site_;
    std::string rf_site_1_;
    std::string rf_site_2_;
};
