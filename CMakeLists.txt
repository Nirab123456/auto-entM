# ---------------------------
# Auto-clone Arduino libraries helper
# ---------------------------
# Usage: call ensure_arduino_library(<name> <git_url> [<tag_or_branch>] [DEST_SUBDIR])
#
# Example:
#   ensure_arduino_library("WiFiManager" "https://github.com/tzapu/WiFiManager.git" "2.0.17")
#
function(ensure_arduino_library libname giturl)
  set(options)
  set(oneValueArgs TAG DEST_SUBDIR)
  set(multiValueArgs)
  cmake_parse_arguments(ENSURE "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

  # Where we try to put the library by default: project-root/main/libraries/<libname>
  if(ENSURE_DEST_SUBDIR)
    set(_dest_dir "${CMAKE_SOURCE_DIR}/${ENSURE_DEST_SUBDIR}/${libname}")
  else()
    set(_dest_dir "${CMAKE_SOURCE_DIR}/main/libraries/${libname}")
  endif()

  if(EXISTS "${_dest_dir}")
    message(STATUS "Library '${libname}' already present at ${_dest_dir}")
    return()
  endif()

  # If git is not available or network fails, we produce a clear message but continue
  find_program(_GIT_EXECUTABLE git)
  if(NOT _GIT_EXECUTABLE)
    message(WARNING "Git not found: cannot auto-clone ${libname}. Please clone manually into ${_dest_dir}")
    return()
  endif()

  message(STATUS "Cloning ${libname} into ${_dest_dir} from ${giturl}...")

  # Build clone command. If TAG provided, use --branch <TAG> --depth 1 for speed.
  if(ENSURE_TAG)
    execute_process(
      COMMAND ${_GIT_EXECUTABLE} clone --depth 1 --branch ${ENSURE_TAG} ${giturl} "${_dest_dir}"
      RESULT_VARIABLE _git_rc
      OUTPUT_VARIABLE _git_out
      ERROR_VARIABLE _git_err
      WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
      )

  else()
    execute_process(
      COMMAND ${_GIT_EXECUTABLE} clone --depth 1 ${giturl} "${_dest_dir}"
      RESULT_VARIABLE _git_rc
      OUTPUT_VARIABLE _git_out
      ERROR_VARIABLE _git_err
      WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
      )
  endif()

  if(NOT _git_rc EQUAL 0)
    message(WARNING "Failed to clone ${libname} (${giturl}) -> ${_dest_dir}.
  git return code: ${_git_rc}
  git stdout: ${_git_out}
  git stderr: ${_git_err}
Please clone manually and re-run idf.py reconfigure.")
  else()
    message(STATUS "Successfully cloned ${libname}.")
  endif()
endfunction()



ensure_arduino_library("WiFiManager" "https://github.com/tzapu/WiFiManager.git" "2.0.17")



cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(auto_entm_aurdino_as_tool)
