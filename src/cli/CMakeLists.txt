# lockstep — the CLI. A thin client of the daemon socket (see DESIGN.md).
add_executable(lockstep main.cpp)
target_link_libraries(lockstep PRIVATE lockstep_common)
