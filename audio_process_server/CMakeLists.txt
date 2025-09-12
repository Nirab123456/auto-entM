cmake_minimum_required(VERSION 3.12)
project(audio_process_server VERSION 1.0 LANGUAGES CXX)

# Allow user to set a Drogon installation prefix on cmake command line:
#
#    cmake -S . -B build -DDROGON_ROOT=/opt/drogon
#
if (DEFINED DROGON_ROOT AND NOT "${DROGON_ROOT}" STREQUAL "")
  list(APPEND CMAKE_PREFIX_PATH "${DROGON_ROOT}")
  message(STATUS "DROGON_ROOT set: ${DROGON_ROOT} -> appended to CMAKE_PREFIX_PATH")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Default to Release if not specified
if (NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Find Drogon (CONFIG mode preferred)
find_package(Drogon REQUIRED CONFIG)
if (NOT TARGET Drogon::Drogon)
  message(FATAL_ERROR "Drogon found but Drogon::Drogon target is not available. Check your Drogon installation.")
endif()

# Threads (pthread)
find_package(Threads REQUIRED)

# Optionally try to find JsonCpp (Drogon already uses a Json library, but link explicitly if available)
find_package(JsonCpp QUIET)

# Source files: receiver + webserver combined into a single binary.
set(RECEIVER_SOURCE
    ${CMAKE_SOURCE_DIR}/esp_receiver/esp_audio_reciver.cpp
)

# Verify source files exist to give useful error messages early
if (NOT EXISTS ${RECEIVER_SOURCE})
  message(FATAL_ERROR "Required source file not found: ${RECEIVER_SOURCE}")
endif()

# Create single executable containing both receiver and web UI code
add_executable(esp_receiver_combined
    ${RECEIVER_SOURCE}
)

# Make sure the compiler can find local headers if any
target_include_directories(esp_receiver_combined PRIVATE
    ${CMAKE_SOURCE_DIR}/web_server
    ${CMAKE_SOURCE_DIR}/esp_receiver
)

# Link Drogon and threads
target_link_libraries(esp_receiver_combined PRIVATE
    Drogon::Drogon
    Threads::Threads
)

# If JsonCpp was found, link it as well (helps if your code includes <json/json.h>)
if (TARGET JsonCpp::JsonCpp)
  target_link_libraries(esp_receiver_combined PRIVATE JsonCpp::JsonCpp)
  message(STATUS "Linking against JsonCpp")
elseif (JsonCpp_FOUND)
  # Some older find modules set JsonCpp_FOUND but not the target; try to link by variable
  if (DEFINED JsonCpp_LIBRARY)
    target_link_libraries(esp_receiver_combined PRIVATE ${JsonCpp_LIBRARY})
    target_include_directories(esp_receiver_combined PRIVATE ${JsonCpp_INCLUDE_DIR})
    message(STATUS "Linking against JsonCpp (legacy variables)")
  else()
    message(WARNING "JsonCpp found but no target/library variable available; your code may fail to link if it needs jsoncpp.")
  endif()
else()
  message(STATUS "JsonCpp not found. Drogon usually provides JSON support; proceed.")
endif()

# Set output directory for the binary (bin/)
set_target_properties(esp_receiver_combined PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# Copy static web assets into build/bin/static after build so newFileResponse("static/...") works.
set(STATIC_SOURCE_DIR ${CMAKE_SOURCE_DIR}/web_server/static)
set(STATIC_DEST_DIR ${CMAKE_BINARY_DIR}/bin/static)

if (EXISTS ${STATIC_SOURCE_DIR})
  add_custom_command(TARGET esp_receiver_combined POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E make_directory ${STATIC_DEST_DIR}
      COMMAND ${CMAKE_COMMAND} -E copy_directory ${STATIC_SOURCE_DIR} ${STATIC_DEST_DIR}
      COMMENT "Copying static web assets to ${STATIC_DEST_DIR}"
  )
else()
  message(WARNING "Static web directory not found: ${STATIC_SOURCE_DIR}. The web UI will 404 for static files unless you create this directory.")
endif()

# Optional install rules
install(TARGETS esp_receiver_combined DESTINATION bin)
if (EXISTS ${STATIC_SOURCE_DIR})
  install(DIRECTORY ${STATIC_SOURCE_DIR} DESTINATION share/${PROJECT_NAME}/static)
endif()

message(STATUS "Configuration complete.")
message(STATUS "Build with: cmake -S . -B build -DDROGON_ROOT=/path/to/drogon && cmake --build build -- -j")
message(STATUS "After build the executable will be at: ${CMAKE_BINARY_DIR}/bin/esp_receiver_combined")
