#pragma once

#include <map>
#include <string>
#include <vector>

namespace lockstep {

struct Config {
    std::vector<std::string> repos;  // absolute, tilde-expanded repo paths
    // Declared dependencies, keyed by repo name (see repo_name()): a repo entry
    // written as {"path": ..., "depends_on": ["uw-core"]} lands here.
    std::map<std::string, std::vector<std::string>> depends_on;
    std::string machine;             // this machine's identity (default: hostname)
    std::string rendezvous_dir;      // file-backend rendezvous dir (tests/interim)
    std::string rendezvous_repo;     // git clone of the private rendezvous repo
    std::string key_path;            // shared AEAD key file (tilde-expanded)
    int tick_seconds = 0;            // background publish/fetch interval; 0 = default
};

// Logical name of a repo path — its basename. This is the key used to match the
// same repo across machines whose absolute paths differ.
std::string repo_name(const std::string& path);

// Path to the config file. Honors $LOCKSTEP_CONFIG, else
// $XDG_CONFIG_HOME/lockstep/config.json, else $HOME/.config/lockstep/config.json.
std::string config_path();

// Load and parse config_path(). A missing file yields an empty Config (not an
// error) — the daemon simply watches nothing until repos are added. On a parse
// error, `error` (if non-null) is set and an empty Config returned.
Config load_config(std::string* error = nullptr);

}  // namespace lockstep
