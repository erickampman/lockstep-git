#pragma once

#include <optional>
#include <string>
#include <vector>

#include "config.h"

namespace lockstep {

// A submodule of a watched repo that is itself a watched repo (matched by the
// submodule URL's basename, e.g. ".../uw-core.git" -> "uw-core").
struct WatchedSubmodule {
    std::string dep_name;   // watched repo name the submodule refers to
    std::string sub_path;   // submodule path inside the parent, e.g. "external/uw-core"
};

// Submodules of `repo_path` (per its .gitmodules) that name a watched repo.
std::vector<WatchedSubmodule> watched_submodules(const Config& cfg,
                                                 const std::string& repo_path);

// Path of the watched repo named `name`, if any.
std::optional<std::string> find_repo_path(const Config& cfg, const std::string& name);

// Everything `name` depends on: config `depends_on` plus detected submodules.
// Deduplicated, never includes `name` itself.
std::vector<std::string> dependencies(const Config& cfg, const std::string& name);

// A parent repo's committed submodule pin vs the standalone clone of that dep.
struct PinDrift {
    std::string dep_name;
    std::string sub_path;
    int pin_only = 0;       // commits in the pin that the clone's HEAD lacks
    int clone_only = 0;     // commits in the clone's HEAD that the pin lacks
    bool unknown = false;   // pin commit isn't in the clone at all (fetch/pull it)
};

// Pin drift for every watched submodule of `repo_path` whose pin differs from
// the standalone clone's HEAD. Empty when everything matches.
std::vector<PinDrift> pin_drift(const Config& cfg, const std::string& repo_path);

}  // namespace lockstep
