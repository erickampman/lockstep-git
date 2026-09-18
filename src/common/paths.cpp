#include "paths.h"

#include <sys/stat.h>

#include <cstdlib>
#include <filesystem>

namespace lockstep {

namespace {

std::string env_or_empty(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

}  // namespace

std::string runtime_dir() {
    if (std::string xdg = env_or_empty("XDG_RUNTIME_DIR"); !xdg.empty()) {
        return xdg + "/lockstep";
    }
    std::string home = env_or_empty("HOME");
    // If even $HOME is unset something is very wrong; fall back to cwd-relative
    // so callers still get a usable (if odd) path rather than an empty string.
    if (home.empty()) home = ".";
    return home + "/.lockstep";
}

std::string socket_path() {
    if (std::string override = env_or_empty("LOCKSTEP_SOCKET"); !override.empty()) {
        return override;
    }
    std::string dir = runtime_dir();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    // Best-effort tighten to 0700; ignore failure (dir may already be correct).
    ::chmod(dir.c_str(), 0700);
    return dir + "/daemon.sock";
}

}  // namespace lockstep
