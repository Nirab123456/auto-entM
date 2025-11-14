# components/ext_arduino_library/CMakeLists.txt
# A small wrapper component that exposes include dirs for each library
cmake_minimum_required(VERSION 3.16)

# Root of this component folder
set(_THIS_COMPONENT_DIR ${CMAKE_CURRENT_LIST_DIR})

# Collect immediate subdirectories (each subdir is expected to be a library folder)
file(GLOB CHILD_DIRS RELATIVE "${_THIS_COMPONENT_DIR}" "${_THIS_COMPONENT_DIR}/*")

# Build a list of include directories to expose to the compiler
set(_EXPOSE_INCLUDES "")
foreach(_d IN LISTS CHILD_DIRS)
  if(IS_DIRECTORY "${_THIS_COMPONENT_DIR}/${_d}")
    # prefer common header locations: src, include, or the folder itself
    if(EXISTS "${_THIS_COMPONENT_DIR}/${_d}/src")
      list(APPEND _EXPOSE_INCLUDES "${_THIS_COMPONENT_DIR}/${_d}/src")
    elseif(EXISTS "${_THIS_COMPONENT_DIR}/${_d}/include")
      list(APPEND _EXPOSE_INCLUDES "${_THIS_COMPONENT_DIR}/${_d}/include")
    else()
      list(APPEND _EXPOSE_INCLUDES "${_THIS_COMPONENT_DIR}/${_d}")
    endif()
  endif()
endforeach()

message(STATUS "ext_arduino_library: exposing includes: ${_EXPOSE_INCLUDES}")

# Register a no-SRCS component that just provides include directories and depends on arduino core
# (IDF will treat it as a component so ordering and REQUIRES work correctly)
idf_component_register(
  INCLUDE_DIRS ${_EXPOSE_INCLUDES}
)
