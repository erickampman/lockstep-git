# lockstep — the CLI. A thin client of the daemon socket (see DESIGN.md).

# The git hook is compiled in so `lockstep install` can write it into repos
# without knowing where the source tree is. Re-configure when it changes.
set(LOCKSTEP_HOOK_FILE ${PROJECT_SOURCE_DIR}/hooks/pre-commit)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${LOCKSTEP_HOOK_FILE})
file(READ ${LOCKSTEP_HOOK_FILE} LOCKSTEP_HOOK_SCRIPT)
configure_file(hook_script.h.in ${CMAKE_CURRENT_BINARY_DIR}/hook_script.h @ONLY)

add_executable(lockstep main.cpp install.cpp diff.cpp)
target_include_directories(lockstep PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(lockstep PRIVATE lockstep_common)
