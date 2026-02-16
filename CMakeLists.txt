cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(WhiteListDetector)

set(EXTRA_COMPONENT_DIRS ./components)

# Create a SPIFFS image from the 'html' directory and flash it to the 'storage' partition
# We place it here with a full path for debugging purposes.
spiffs_create_partition_image(storage "${CMAKE_SOURCE_DIR}/components/web_server/html" DEPENDS "${CMAKE_SOURCE_DIR}/components/web_server/html/index.html")
