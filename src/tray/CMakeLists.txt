# lockstep-tray — optional Qt system-tray UI.
#
# Guarded: if Qt6 isn't found the core (daemon + CLI) still builds, and this
# target is simply skipped. To enable it, install Qt and configure with
#   cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH="$(brew --prefix qt)"
find_package(Qt6 QUIET COMPONENTS Widgets)
if(NOT Qt6_FOUND)
    message(STATUS "Qt6 not found — skipping lockstep-tray (core unaffected). "
                   "Pass -DCMAKE_PREFIX_PATH=\"$(brew --prefix qt)\" to enable it.")
    return()
endif()

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)  # compiles resources.qrc (the brand icon) into the binary
add_executable(lockstep-tray main.cpp resources.qrc)
target_link_libraries(lockstep-tray PRIVATE lockstep_common Qt6::Widgets)
message(STATUS "lockstep-tray enabled (Qt ${Qt6_VERSION})")
