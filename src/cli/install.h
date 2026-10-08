#pragma once

// `lockstep install` / `lockstep uninstall`: put the CLI + daemon on
// ~/.local/bin, write the git hooks into every watched repo, and register the
// daemon with the platform's user service manager (systemd --user on Linux, a
// LaunchAgent on macOS). Both are idempotent.
namespace lockstep::cli {

int cmd_install();
int cmd_uninstall();

}  // namespace lockstep::cli
