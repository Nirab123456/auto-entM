# main/CMakeLists.txt
# Collect all .c/.cpp files under main/ (root + subfolders)
file(GLOB_RECURSE MAIN_SRCS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cc"
)

# Public include dirs for this component
set(MAIN_INCLUDES
    "${CMAKE_CURRENT_SOURCE_DIR}"           # main root (so main_helper.h, etc. are visible)
    "${CMAKE_CURRENT_SOURCE_DIR}/headers"   # headers/ folder
    "${CMAKE_CURRENT_SOURCE_DIR}/programs"  # programs/ folder (your headers live here)
)

# Force C++20 so std::span is available (optional but recommended)
# This affects files compiled after this line.
set(CMAKE_CXX_STANDARD_REQUIRED ON)

idf_component_register(
    SRCS ${MAIN_SRCS}
    INCLUDE_DIRS ${MAIN_INCLUDES}
)
