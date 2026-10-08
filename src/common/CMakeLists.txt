# Shared code between lockstepd and the lockstep CLI: socket path resolution,
# the wire protocol, a thin Unix-socket transport, git inspection, config, and
# the rendezvous crypto/blob layer.
find_package(PkgConfig REQUIRED)
pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium)

add_library(lockstep_common STATIC
    blob.cpp
    config.cpp
    crypto.cpp
    deps.cpp
    git.cpp
    ipc.cpp
    paths.cpp
    rendezvous_file.cpp
    rendezvous_github.cpp
    subprocess.cpp)

target_include_directories(lockstep_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(lockstep_common PUBLIC
    nlohmann_json::nlohmann_json
    PkgConfig::SODIUM)
