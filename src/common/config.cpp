#include "config.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

namespace lockstep {

namespace {

std::string env_or_empty(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

// Expand a leading "~" or "~/..." to $HOME. Other paths pass through unchanged.
std::string expand_tilde(const std::string& p) {
    if (p.empty() || p[0] != '~') return p;
    std::string home = env_or_empty("HOME");
    if (home.empty()) return p;
    if (p.size() == 1) return home;         // "~"
    if (p[1] == '/') return home + p.substr(1);  // "~/foo"
    return p;  // "~user" not supported; leave as-is
}

}  // namespace

std::string repo_name(const std::string& path) {
    std::string name = std::filesystem::path(path).filename().string();
    if (name.empty()) name = std::filesystem::path(path).parent_path().filename().string();
    return name.empty() ? path : name;  // trailing-slash guard
}

std::string config_path() {
    if (std::string o = env_or_empty("LOCKSTEP_CONFIG"); !o.empty()) return o;
    if (std::string xdg = env_or_empty("XDG_CONFIG_HOME"); !xdg.empty())
        return xdg + "/lockstep/config.json";
    std::string home = env_or_empty("HOME");
    if (home.empty()) home = ".";
    return home + "/.config/lockstep/config.json";
}

Config load_config(std::string* error) {
    Config cfg;
    std::ifstream in(config_path());
    if (!in) return cfg;  // missing file is fine: watch nothing

    std::stringstream ss;
    ss << in.rdbuf();
    auto json = nlohmann::json::parse(ss.str(), nullptr, /*allow_exceptions=*/false);
    if (json.is_discarded()) {
        if (error) *error = "config is not valid JSON: " + config_path();
        return cfg;
    }

    if (auto it = json.find("repos"); it != json.end() && it->is_array()) {
        for (const auto& r : *it) {
            // Each entry is either "path" or {"path": ..., "depends_on": [names]}.
            if (r.is_string()) {
                cfg.repos.push_back(expand_tilde(r.get<std::string>()));
            } else if (r.is_object() && r.contains("path") && r["path"].is_string()) {
                std::string path = expand_tilde(r["path"].get<std::string>());
                cfg.repos.push_back(path);
                if (auto d = r.find("depends_on"); d != r.end() && d->is_array()) {
                    auto& deps = cfg.depends_on[repo_name(path)];
                    for (const auto& dep : *d)
                        if (dep.is_string()) deps.push_back(dep.get<std::string>());
                }
            }
        }
    }
    if (auto it = json.find("machine"); it != json.end() && it->is_string()) {
        cfg.machine = it->get<std::string>();
    }
    if (auto it = json.find("rendezvous_dir"); it != json.end() && it->is_string()) {
        cfg.rendezvous_dir = expand_tilde(it->get<std::string>());
    }
    if (auto it = json.find("rendezvous_repo"); it != json.end() && it->is_string()) {
        cfg.rendezvous_repo = expand_tilde(it->get<std::string>());
    }
    if (auto it = json.find("key_path"); it != json.end() && it->is_string()) {
        cfg.key_path = expand_tilde(it->get<std::string>());
    }
    return cfg;
}

}  // namespace lockstep
