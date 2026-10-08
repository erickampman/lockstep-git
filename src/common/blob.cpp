#include "blob.h"

#include <unistd.h>

#include <ctime>

#include "git.h"

namespace lockstep {

std::string machine_id(const Config& cfg) {
    if (!cfg.machine.empty()) return cfg.machine;
    char host[256];
    if (::gethostname(host, sizeof(host)) == 0) {
        host[sizeof(host) - 1] = '\0';
        return host;
    }
    return "unknown";
}

MachineBlob gather(const Config& cfg) {
    MachineBlob b;
    b.machine = machine_id(cfg);
    b.timestamp = static_cast<int64_t>(std::time(nullptr));

    for (const auto& path : cfg.repos) {
        RepoState s = inspect(path);
        RepoBrief r;
        r.name = repo_name(path);
        r.branch = s.branch;
        r.dirty = s.dirty;
        r.has_upstream = s.has_upstream;
        r.ahead = s.ahead;
        r.behind = s.behind;
        r.clean = is_clean(s);
        r.readable = s.error.empty() && s.is_repo;
        b.repos.push_back(std::move(r));
    }
    return b;
}

nlohmann::json to_json(const MachineBlob& b) {
    nlohmann::json repos = nlohmann::json::array();
    for (const auto& r : b.repos) {
        repos.push_back({{"name", r.name},
                         {"branch", r.branch},
                         {"dirty", r.dirty},
                         {"has_upstream", r.has_upstream},
                         {"ahead", r.ahead},
                         {"behind", r.behind},
                         {"clean", r.clean},
                         {"readable", r.readable}});
    }
    return {{"v", 1},
            {"machine", b.machine},
            {"timestamp", b.timestamp},
            {"repos", std::move(repos)}};
}

std::optional<MachineBlob> blob_from_json(const nlohmann::json& j) {
    if (!j.is_object()) return std::nullopt;
    MachineBlob b;
    b.machine = j.value("machine", "");
    b.timestamp = j.value("timestamp", int64_t{0});
    if (auto it = j.find("repos"); it != j.end() && it->is_array()) {
        for (const auto& e : *it) {
            RepoBrief r;
            r.name = e.value("name", "");
            r.branch = e.value("branch", "");
            r.dirty = e.value("dirty", 0);
            r.has_upstream = e.value("has_upstream", false);
            r.ahead = e.value("ahead", 0);
            r.behind = e.value("behind", 0);
            r.clean = e.value("clean", true);
            r.readable = e.value("readable", true);
            b.repos.push_back(std::move(r));
        }
    }
    return b;
}

}  // namespace lockstep
