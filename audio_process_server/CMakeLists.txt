cmake_minimum_required(VERSION 3.12)
project(esp_receiver_and_web VERSION 1.0 LANGUAGES CXX)

# Silence the CMP0144 warning by setting it explicitly.
# CMP0144: find_package uses upper-case <PACKAGENAME>_ROOT variables.
if(POLICY CMP0144)
  cmake_policy(SET CMP0144 NEW)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Default to Release if not specified
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

#
# Allow passing a Drogon install prefix. Use a proper CACHE PATH variable so
# users can provide e.g. -DDROGON_ROOT=/opt/drogon on the cmake command line.
#
set(DROGON_ROOT "" CACHE PATH "Path to Drogon installation prefix (contains lib/cmake/Drogon)")

if(DROGON_ROOT)
  list(APPEND CMAKE_PREFIX_PATH "${DROGON_ROOT}")
  message(STATUS "DROGON_ROOT provided, added to CMAKE_PREFIX_PATH: ${DROGON_ROOT}")
endif()

# Try finding Drogon. Prefer CONFIG mode to pick up DrogonConfig.cmake
find_package(Drogon REQUIRED CONFIG)

if (DEFINED Drogon_DIR)
  message(STATUS "Drogon CMake config found at: ${Drogon_DIR}")
endif()

# Receiver and webserver sources
set(RECEIVER_SRC
    esp_receiver/esp_audio_reciver.cpp
)

set(WEBSERVER_SRC
    web_server/main.cpp
)

add_executable(esp_receiver
    ${RECEIVER_SRC}
    ${WEBSERVER_SRC}
)

# Make sure our local headers are visible
target_include_directories(esp_receiver PRIVATE
    ${CMAKE_SOURCE_DIR}/web_server
    ${CMAKE_SOURCE_DIR}/esp_receiver
)

# Link Drogon (imported target) and threads
target_link_libraries(esp_receiver PRIVATE Drogon::Drogon)
find_package(Threads REQUIRED)
target_link_libraries(esp_receiver PRIVATE Threads::Threads)

# Copy static web assets to build output
set(STATIC_SRC_DIR ${CMAKE_SOURCE_DIR}/web_server/static)
set(STATIC_DEST_DIR ${CMAKE_BINARY_DIR}/static)
add_custom_command(TARGET esp_receiver PRE_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${STATIC_DEST_DIR}
)
add_custom_command(TARGET esp_receiver POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${STATIC_SRC_DIR} ${STATIC_DEST_DIR}
    COMMENT "Copying static web assets to build/static"
)

install(TARGETS esp_receiver DESTINATION bin)
install(DIRECTORY ${STATIC_SRC_DIR} DESTINATION share/${PROJECT_NAME}/static)
