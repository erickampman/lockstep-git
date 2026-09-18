#pragma once

#include <string>

namespace lockstep {

// Absolute path to the daemon's Unix socket. Honors $LOCKSTEP_SOCKET for tests
// and overrides; otherwise a per-user path under the runtime/state dir:
//   - $XDG_RUNTIME_DIR/lockstep/daemon.sock   (Linux, when set)
//   - $HOME/.lockstep/daemon.sock             (fallback, incl. macOS)
// The parent directory is created (0700) on demand by the daemon.
std::string socket_path();

// Directory portion of socket_path(); created 0700 if missing.
std::string runtime_dir();

}  // namespace lockstep
