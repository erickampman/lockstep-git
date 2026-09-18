cmake_minimum_required(VERSION 3.24)

project(lockstep-git
    VERSION 0.1.0
    DESCRIPTION "Cross-machine git sync guard: warn/block at commit time when the other machine has pending work"
    LANGUAGES CXX)

# --- Global settings -------------------------------------------------------

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile_commands.json for clangd (used on both platforms, see DESIGN.md).
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Default to a Debug build if the user (or IDE) didn't pick one.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

if(NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

# --- Dependencies ----------------------------------------------------------

# nlohmann/json: header-only, pulled via FetchContent so there's no brew
# dependency for the core build. (Qt, libsodium, libgit2 come later.)
include(FetchContent)
FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    GIT_SHALLOW TRUE)
set(JSON_BuildTests OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(nlohmann_json)

# --- Targets ---------------------------------------------------------------

add_subdirectory(src/common)
add_subdirectory(src/daemon)
add_subdirectory(src/cli)

# --- Summary ---------------------------------------------------------------

message(STATUS "lockstep-git ${PROJECT_VERSION} — build type: ${CMAKE_BUILD_TYPE}")
