#pragma once

#include <string>
#include <vector>

namespace lockstep {

struct Config {
    std::vector<std::string> repos;  // absolute, tilde-expanded repo paths
};

// Path to the config file. Honors $LOCKSTEP_CONFIG, else
// $XDG_CONFIG_HOME/lockstep/config.json, else $HOME/.config/lockstep/config.json.
std::string config_path();

// Load and parse config_path(). A missing file yields an empty Config (not an
// error) — the daemon simply watches nothing until repos are added. On a parse
// error, `error` (if non-null) is set and an empty Config returned.
Config load_config(std::string* error = nullptr);

}  // namespace lockstep
