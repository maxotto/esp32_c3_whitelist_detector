# CMakeLists.txt for wifi_controller component

set(COMPONENT_SRCDIRS "src")
set(COMPONENT_ADD_INCLUDEDIRS "include")

# Register the component with its requirements
idf_component_register(
    SRCS "src/wifi_controller.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_event esp_netif nvs_flash
)