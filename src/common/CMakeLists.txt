# Shared code between lockstepd and the lockstep CLI: socket path resolution,
# the wire protocol, and a thin blocking Unix-socket transport.
add_library(lockstep_common STATIC
    ipc.cpp
    paths.cpp)

target_include_directories(lockstep_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(lockstep_common PUBLIC nlohmann_json::nlohmann_json)
