# CMakeLists.txt for website_detector component

set(COMPONENT_SRCDIRS "src")
set(COMPONENT_ADD_INCLUDEDIRS "include")

# Register the component with its requirements
idf_component_register(
    SRCS "src/website_detector.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_event esp_netif nvs_flash lwip
)