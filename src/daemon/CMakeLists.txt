# lockstepd — the per-machine daemon. Plain foreground process: logs to
# stdout/stderr, handles SIGTERM/SIGINT cleanly, does NOT self-daemonize
# (launchd/systemd-user supervise it). See DESIGN.md.
add_executable(lockstepd main.cpp)
target_link_libraries(lockstepd PRIVATE lockstep_common)
