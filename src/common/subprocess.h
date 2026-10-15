#pragma once

#include <string>
#include <vector>

namespace lockstep {

struct CmdResult {
    bool spawned = false;  // false if the process couldn't be started at all
    int exit_code = -1;    // process exit status (valid when spawned)
    std::string out;       // captured stdout (stderr is discarded)
    bool timed_out = false;  // killed for exceeding timeout_seconds
};

// Run argv[0] with argv as arguments, WITHOUT a shell (no injection surface),
// capturing stdout. stderr is sent to /dev/null. Blocks until the child exits,
// or — when timeout_seconds > 0 — until the deadline passes, at which point the
// child's whole process group (e.g. git and its remote helpers) is SIGKILLed.
// argv must be non-empty; argv[0] is resolved via PATH (execvp).
CmdResult run(const std::vector<std::string>& argv, int timeout_seconds = 0);

}  // namespace lockstep
