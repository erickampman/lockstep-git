#pragma once

#include <string>
#include <vector>

namespace lockstep {

struct CmdResult {
    bool spawned = false;  // false if the process couldn't be started at all
    int exit_code = -1;    // process exit status (valid when spawned)
    std::string out;       // captured stdout (stderr is discarded)
};

// Run argv[0] with argv as arguments, WITHOUT a shell (no injection surface),
// capturing stdout. stderr is sent to /dev/null. Blocks until the child exits.
// argv must be non-empty; argv[0] is resolved via PATH (execvp).
CmdResult run(const std::vector<std::string>& argv);

}  // namespace lockstep
