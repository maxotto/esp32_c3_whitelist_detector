# Get the list of source files for the component
set(srcs "web_server.cpp" "config_manager.cpp")

# Register the component with the build system
# By adding ".", we make the headers in this directory (like web_server.h) public.
idf_component_register(SRCS "${srcs}"
                       INCLUDE_DIRS "."
                       REQUIRES spiffs nvs_flash)

# Create a SPIFFS image from this component's 'html' directory and flash it to the 'storage' partition
spiffs_create_partition_image(storage html)
