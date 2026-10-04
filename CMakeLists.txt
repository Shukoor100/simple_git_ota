cmake_minimum_required(VERSION 3.16)

# Firmware version = integer build number written by CI into fw_version.txt.
# Local builds (no file) get version 0, so they will update to the latest CI build.
# Must be set BEFORE project.cmake is included.
set(FW_VERSION_FILE "${CMAKE_CURRENT_LIST_DIR}/fw_version.txt")
if(EXISTS "${FW_VERSION_FILE}")
    file(STRINGS "${FW_VERSION_FILE}" PROJECT_VER LIMIT_COUNT 1)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${FW_VERSION_FILE}")
else()
    set(PROJECT_VER "0")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_ota)
