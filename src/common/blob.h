#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "config.h"

namespace lockstep {

// Publishable per-repo summary. Deliberately metadata-only: counts and
// branch/ahead-behind, NOT filenames (decision A + security model). `name` is a
// logical key used to match the same repo across machines whose absolute paths
// differ (e.g. ~/dev/foo on mac vs ~/src/foo on linux) — the path basename.
struct RepoBrief {
    std::string name;
    std::string branch;
    int dirty = 0;
    bool has_upstream = false;
    int ahead = 0;
    int behind = 0;
    bool clean = true;        // no dirty + not ahead
    bool readable = true;     // false if the path wasn't a git work tree
};

// One machine's published state.
struct MachineBlob {
    std::string machine;                 // publishing machine's identity
    int64_t timestamp = 0;               // unix seconds at publish time
    std::vector<RepoBrief> repos;
};

// Identity of this machine: config `machine` if set, else the hostname.
std::string machine_id(const Config& cfg);

// Inspect all configured repos right now and build this machine's blob.
MachineBlob gather(const Config& cfg);

nlohmann::json to_json(const MachineBlob& b);
std::optional<MachineBlob> blob_from_json(const nlohmann::json& j);

}  // namespace lockstep
