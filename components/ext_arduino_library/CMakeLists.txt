cmake_minimum_required(VERSION 3.16)

# Use the directory of this file
set(_THIS_DIR ${CMAKE_CURRENT_LIST_DIR})

# Collect WiFiManager sources (.cpp/.c) recursively (covers root and src/ folders)
file(GLOB_RECURSE WIFI_SRCS
     RELATIVE "${_THIS_DIR}"
     "${_THIS_DIR}/*.cpp"
     "${_THIS_DIR}/*.c"
)

message(STATUS "WiFiManager component: found sources: ${WIFI_SRCS}")

# Expose include dirs (common locations)
set(WIFI_INCLUDES "")
if(EXISTS "${_THIS_DIR}/src")
  list(APPEND WIFI_INCLUDES "${_THIS_DIR}/src")
endif()
if(EXISTS "${_THIS_DIR}/include")
  list(APPEND WIFI_INCLUDES "${_THIS_DIR}/include")
endif()
# Always add root folder (some libraries keep headers here)
list(APPEND WIFI_INCLUDES "${_THIS_DIR}")

# Register the component so IDF builds it.
# Add REQUIRES: arduino so Arduino symbols (String, etc) are available.
# Add esp_wifi, esp_event, nvs_flash, esp_log to pull runtime dependencies.
idf_component_register(
    SRCS ${WIFI_SRCS}
    INCLUDE_DIRS ${WIFI_INCLUDES}
  REQUIRES
    arduino-esp32-3.3.4     # ensure Arduino core is linked (provides String, WebServer, etc)
    nvs_flash   # if WiFiManager uses preferences
    esp_wifi    # if needed (safe to require)
)
