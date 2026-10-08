#pragma once

// `lockstep install` / `lockstep uninstall`: put the CLI + daemon on
// ~/.local/bin, write the git hooks into every watched repo, and register the
// daemon with the platform's user service manager (systemd --user on Linux, a
// LaunchAgent on macOS). Both are idempotent.
#include <string>

namespace lockstep::cli {

int cmd_install();
int cmd_uninstall();

// `lockstep add <path>` / `lockstep remove <path|name>`: edit the watched-repo
// list in config.json (preserving key order) and install/remove the hooks in
// that repo. The daemon reloads config on its next tick, so no restart needed.
int cmd_add(const std::string& path);
int cmd_remove(const std::string& path_or_name);

}  // namespace lockstep::cli
