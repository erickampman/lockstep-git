#include "deps.h"

#include <algorithm>
#include <cstdlib>
#include <map>
#include <sstream>

#include "subprocess.h"

namespace lockstep {

namespace {

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
        s.pop_back();
    return s;
}

// "https://github.com/x/uw-core.git", "git@github.com:x/uw-core", ".../uw-core/"
// -> "uw-core".
std::string url_basename(std::string url) {
    while (!url.empty() && url.back() == '/') url.pop_back();
    if (url.size() > 4 && url.compare(url.size() - 4, 4, ".git") == 0)
        url.resize(url.size() - 4);
    auto cut = url.find_last_of("/:");
    return cut == std::string::npos ? url : url.substr(cut + 1);
}

}  // namespace

std::optional<std::string> find_repo_path(const Config& cfg, const std::string& name) {
    for (const auto& path : cfg.repos)
        if (repo_name(path) == name) return path;
    return std::nullopt;
}

std::vector<WatchedSubmodule> watched_submodules(const Config& cfg,
                                                 const std::string& repo_path) {
    // Lines look like "submodule.<name>.path external/uw-core" and
    // "submodule.<name>.url https://...". <name> may itself contain dots, so
    // split the key on its LAST dot.
    CmdResult r = run({"git", "-C", repo_path, "config", "-f", ".gitmodules",
                       "--get-regexp", "^submodule\\..*\\.(path|url)$"});
    if (!r.spawned || r.exit_code != 0) return {};

    std::map<std::string, std::pair<std::string, std::string>> subs;  // name -> {path, url}
    std::istringstream in(r.out);
    std::string line;
    while (std::getline(in, line)) {
        auto sp = line.find(' ');
        if (sp == std::string::npos) continue;
        std::string key = line.substr(0, sp), value = trim(line.substr(sp + 1));
        auto dot = key.rfind('.');
        if (dot == std::string::npos || dot <= 10) continue;  // 10 == len("submodule.")
        std::string sub = key.substr(10, dot - 10), field = key.substr(dot + 1);
        (field == "path" ? subs[sub].first : subs[sub].second) = value;
    }

    std::vector<WatchedSubmodule> out;
    std::string self = repo_name(repo_path);
    for (const auto& [sub, pu] : subs) {
        std::string dep = url_basename(pu.second);
        if (pu.first.empty() || dep.empty() || dep == self) continue;
        if (find_repo_path(cfg, dep)) out.push_back({dep, pu.first});
    }
    return out;
}

std::vector<std::string> dependencies(const Config& cfg, const std::string& name) {
    std::vector<std::string> deps;
    auto add = [&](const std::string& d) {
        if (d != name && std::find(deps.begin(), deps.end(), d) == deps.end())
            deps.push_back(d);
    };
    if (auto it = cfg.depends_on.find(name); it != cfg.depends_on.end())
        for (const auto& d : it->second) add(d);
    if (auto path = find_repo_path(cfg, name))
        for (const auto& s : watched_submodules(cfg, *path)) add(s.dep_name);
    return deps;
}

std::vector<PinDrift> pin_drift(const Config& cfg, const std::string& repo_path) {
    std::vector<PinDrift> out;
    for (const auto& sub : watched_submodules(cfg, repo_path)) {
        auto dep_path = find_repo_path(cfg, sub.dep_name);
        if (!dep_path) continue;

        // The pin as committed in the parent's HEAD (not the submodule checkout).
        CmdResult pin = run({"git", "-C", repo_path, "rev-parse", "HEAD:" + sub.sub_path});
        CmdResult head = run({"git", "-C", *dep_path, "rev-parse", "HEAD"});
        if (!pin.spawned || pin.exit_code != 0 || !head.spawned || head.exit_code != 0)
            continue;
        std::string pin_sha = trim(pin.out), head_sha = trim(head.out);
        if (pin_sha == head_sha) continue;

        PinDrift d{sub.dep_name, sub.sub_path};
        // "<pin-only>\t<clone-only>"; fails if the clone has never seen the pin.
        CmdResult lr = run({"git", "-C", *dep_path, "rev-list", "--count",
                            "--left-right", pin_sha + "..." + head_sha});
        std::string s = trim(lr.out);
        auto tab = s.find('\t');
        if (!lr.spawned || lr.exit_code != 0 || tab == std::string::npos) {
            d.unknown = true;
        } else {
            d.pin_only = std::atoi(s.substr(0, tab).c_str());
            d.clone_only = std::atoi(s.substr(tab + 1).c_str());
        }
        out.push_back(std::move(d));
    }
    return out;
}

}  // namespace lockstep
