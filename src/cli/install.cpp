#include "install.h"

#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "config.h"
#include "hook_script.h"
#include "ipc.h"
#include "paths.h"
#include "subprocess.h"

namespace fs = std::filesystem;

namespace lockstep::cli {

namespace {

const char* const kHookNames[] = {"pre-commit", "pre-push"};
const char* const kBinaries[] = {"lockstep", "lockstepd"};

std::string home() {
    const char* h = std::getenv("HOME");
    return h ? h : "";
}

fs::path bin_dir() { return fs::path(home()) / ".local" / "bin"; }

// "~/..." for display when under $HOME.
std::string pretty(const fs::path& p) {
    std::string s = p.string(), h = home();
    if (!h.empty() && s.compare(0, h.size(), h) == 0 &&
        (s.size() == h.size() || s[h.size()] == '/'))
        return "~" + s.substr(h.size());
    return s;
}

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
        s.pop_back();
    return s;
}

// Directory of this running binary, symlinks resolved (lockstepd is built
// beside it, so that's where the ~/.local/bin links point).
std::optional<fs::path> self_dir() {
    std::error_code ec;
#ifdef __APPLE__
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buf(size, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) != 0) return std::nullopt;
    fs::path p = fs::canonical(buf.c_str(), ec);
#else
    fs::path p = fs::canonical("/proc/self/exe", ec);
#endif
    if (ec) return std::nullopt;
    return p.parent_path();
}

std::optional<std::string> read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return std::nullopt;
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool write_file(const fs::path& p, const std::string& contents, fs::perms mode) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    {
        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << contents;
        if (!out) return false;
    }
    fs::permissions(p, mode, ec);
    return !ec;
}

// A hook is ours if it's any version of the lockstep script (current or older).
bool is_our_hook(const std::string& s) {
    return s.find("lockstep-git") != std::string::npos &&
           s.find("LOCKSTEP_SKIP") != std::string::npos;
}

bool daemon_up() {
    return lockstep::ipc::request(lockstep::socket_path(), {{"cmd", lockstep::ipc::kCmdPing}})
        .has_value();
}

bool wait_for_daemon(int tenths) {
    for (int i = 0; i < tenths; ++i) {
        if (daemon_up()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

bool ok(const CmdResult& r) { return r.spawned && r.exit_code == 0; }

// Hooks directory of a repo, honoring core.hooksPath and worktrees.
std::optional<fs::path> hooks_dir(const std::string& repo) {
    CmdResult r = run({"git", "-C", repo, "rev-parse", "--git-path", "hooks"});
    if (!ok(r)) return std::nullopt;
    fs::path p = trim(r.out);
    return p.is_absolute() ? p : fs::path(repo) / p;
}

// --- Service manager (one implementation per platform) ---------------------

#ifdef __APPLE__

constexpr const char* kLabel = "com.ericlkampman.lockstep";

fs::path service_file() {
    return fs::path(home()) / "Library" / "LaunchAgents" / (std::string(kLabel) + ".plist");
}

std::string gui_domain() { return "gui/" + std::to_string(::getuid()); }

bool service_loaded() {
    return ok(run({"launchctl", "print", gui_domain() + "/" + kLabel}));
}

std::string service_contents() {
    std::string daemon = (bin_dir() / "lockstepd").string();
    std::string log = (fs::path(home()) / "Library" / "Logs" / "lockstep.log").string();
    // launchd starts agents with a bare PATH; add Homebrew so the daemon finds
    // the same git (and credential helper) the shell does.
    return R"(<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>)" + std::string(kLabel) + R"(</string>
    <key>ProgramArguments</key>
    <array>
        <string>)" + daemon + R"(</string>
    </array>
    <key>RunAtLoad</key>
    <true/>
    <key>KeepAlive</key>
    <true/>
    <key>ThrottleInterval</key>
    <integer>10</integer>
    <key>EnvironmentVariables</key>
    <dict>
        <key>PATH</key>
        <string>/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin</string>
    </dict>
    <key>StandardOutPath</key>
    <string>)" + log + R"(</string>
    <key>StandardErrorPath</key>
    <string>)" + log + R"(</string>
</dict>
</plist>
)";
}

// (Re)load the agent so it runs the current binary.
bool service_start(std::string* err) {
    if (service_loaded()) run({"launchctl", "bootout", gui_domain() + "/" + kLabel});
    // bootstrap right after bootout can fail transiently while launchd tears
    // the old job down; retry briefly.
    for (int i = 0; i < 10; ++i) {
        if (ok(run({"launchctl", "bootstrap", gui_domain(), service_file().string()})))
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    *err = "launchctl bootstrap failed (try: launchctl bootstrap " + gui_domain() + " " +
           pretty(service_file()) + ")";
    return false;
}

void service_stop() {
    if (service_loaded()) run({"launchctl", "bootout", gui_domain() + "/" + kLabel});
}

const char* kLogsHint = "logs: ~/Library/Logs/lockstep.log";
const char* kRestartHint =
    "after a rebuild: launchctl kickstart -k gui/$(id -u)/com.ericlkampman.lockstep";

// --- Tray LaunchAgent (separate from the daemon: it needs the GUI session) --

constexpr const char* kTrayLabel = "com.ericlkampman.lockstep.tray";

fs::path tray_service_file() {
    return fs::path(home()) / "Library" / "LaunchAgents" /
           (std::string(kTrayLabel) + ".plist");
}

bool tray_loaded() {
    return ok(run({"launchctl", "print", gui_domain() + "/" + kTrayLabel}));
}

std::string tray_service_contents() {
    std::string bin = (bin_dir() / "lockstep-tray").string();
    std::string log = (fs::path(home()) / "Library" / "Logs" / "lockstep-tray.log").string();
    // KeepAlive only on crash (SuccessfulExit false), so the menu's Quit sticks.
    // PATH includes Homebrew so `lockstep add` (which it spawns) finds git.
    return R"(<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>)" + std::string(kTrayLabel) + R"(</string>
    <key>ProgramArguments</key>
    <array>
        <string>)" + bin + R"(</string>
    </array>
    <key>RunAtLoad</key>
    <true/>
    <key>KeepAlive</key>
    <dict>
        <key>SuccessfulExit</key>
        <false/>
    </dict>
    <key>EnvironmentVariables</key>
    <dict>
        <key>PATH</key>
        <string>/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin</string>
    </dict>
    <key>StandardOutPath</key>
    <string>)" + log + R"(</string>
    <key>StandardErrorPath</key>
    <string>)" + log + R"(</string>
</dict>
</plist>
)";
}

bool tray_start(std::string* err) {
    if (tray_loaded()) run({"launchctl", "bootout", gui_domain() + "/" + kTrayLabel});
    for (int i = 0; i < 10; ++i) {
        if (ok(run({"launchctl", "bootstrap", gui_domain(), tray_service_file().string()})))
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    *err = "launchctl bootstrap (tray) failed";
    return false;
}

void tray_stop() {
    if (tray_loaded()) run({"launchctl", "bootout", gui_domain() + "/" + kTrayLabel});
}

#else  // Linux: systemd --user

fs::path service_file() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    fs::path base = (xdg && *xdg) ? fs::path(xdg) : fs::path(home()) / ".config";
    return base / "systemd" / "user" / "lockstep.service";
}

bool service_loaded() {
    return ok(run({"systemctl", "--user", "is-active", "--quiet", "lockstep.service"}));
}

std::string service_contents() {
    return "[Unit]\n"
           "Description=lockstep-git daemon (cross-machine git sync guard)\n"
           "\n"
           "[Service]\n"
           "ExecStart=" + (bin_dir() / "lockstepd").string() + "\n"
           "Restart=on-failure\n"
           "RestartSec=10\n"
           "\n"
           "[Install]\n"
           "WantedBy=default.target\n";
}

// Enable at login and (re)start so it runs the current binary.
bool service_start(std::string* err) {
    if (!ok(run({"systemctl", "--user", "daemon-reload"}))) {
        *err = "systemctl --user daemon-reload failed (is there a user systemd session?)";
        return false;
    }
    if (!ok(run({"systemctl", "--user", "enable", "lockstep.service"}))) {
        *err = "systemctl --user enable lockstep.service failed";
        return false;
    }
    if (!ok(run({"systemctl", "--user", "restart", "lockstep.service"}))) {
        *err = "systemctl --user restart lockstep.service failed "
               "(see: journalctl --user -u lockstep)";
        return false;
    }
    return true;
}

void service_stop() {
    run({"systemctl", "--user", "disable", "--now", "lockstep.service"});
}

const char* kLogsHint = "logs: journalctl --user -u lockstep";
const char* kRestartHint = "after a rebuild: systemctl --user restart lockstep";

#endif

// --- Shared bits for add/remove --------------------------------------------

fs::perms hook_perms() {
    return fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
           fs::perms::others_read | fs::perms::others_exec;
}
fs::perms config_perms() {
    return fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read |
           fs::perms::others_read;
}

// Symlink ~/.local/bin/<name> -> <srcdir>/<name>, printing the result. Leaves a
// real (non-symlink) file in place rather than clobbering it.
bool link_binary(const fs::path& srcdir, const char* name) {
    fs::path link = bin_dir() / name, target = srcdir / name;
    std::error_code ec;
    fs::create_directories(bin_dir(), ec);
    auto st = fs::symlink_status(link, ec);
    if (fs::exists(st) && !fs::is_symlink(st)) {
        std::printf("  ! %s exists and isn't a symlink; left alone\n", pretty(link).c_str());
        return false;
    }
    fs::remove(link, ec);
    fs::create_symlink(target, link, ec);
    if (ec) {
        std::printf("  ! %s: %s\n", pretty(link).c_str(), ec.message().c_str());
        return false;
    }
    std::printf("  %s -> %s\n", pretty(link).c_str(), pretty(target).c_str());
    return true;
}

// The path string of a "repos" entry — a bare string or {"path": ...}.
std::string entry_path(const nlohmann::ordered_json& e) {
    if (e.is_string()) return e.get<std::string>();
    if (e.is_object() && e.contains("path") && e["path"].is_string())
        return e["path"].get<std::string>();
    return "";
}

std::string expand_home(const std::string& p) {
    if (!p.empty() && p[0] == '~') {
        std::string h = home();
        if (!h.empty()) {
            if (p.size() == 1) return h;
            if (p[1] == '/') return h + p.substr(1);
        }
    }
    return p;
}

bool same_path(const std::string& a, const std::string& b) {
    std::error_code e1, e2;
    fs::path ca = fs::weakly_canonical(expand_home(a), e1);
    fs::path cb = fs::weakly_canonical(expand_home(b), e2);
    if (e1 || e2) return expand_home(a) == expand_home(b);
    return ca == cb;
}

// Write lockstep's hooks into `repo`, printing a per-hook status line.
void install_hooks_report(const std::string& repo) {
    auto hdir = hooks_dir(repo);
    if (!hdir) {
        std::printf("  ! %s is not a git work tree; no hooks installed\n",
                    pretty(repo).c_str());
        return;
    }
    std::string line;
    for (const char* name : kHookNames) {
        fs::path hook = *hdir / name;
        auto existing = read_file(hook);
        std::string state;
        if (existing && !is_our_hook(*existing))
            state = "kept existing (not lockstep's)";
        else if (existing && *existing == kHookScript)
            state = "up to date";
        else if (write_file(hook, kHookScript, hook_perms()))
            state = existing ? "updated" : "installed";
        else
            state = "FAILED to write";
        line += std::string(line.empty() ? "" : ", ") + name + " " + state;
    }
    std::printf("  hooks: %s\n", line.c_str());
}

// Remove lockstep's hooks from `repo` (leaves foreign hooks alone).
void remove_hooks_report(const std::string& repo) {
    auto hdir = hooks_dir(repo);
    if (!hdir) return;
    std::error_code ec;
    std::string line;
    for (const char* name : kHookNames) {
        fs::path hook = *hdir / name;
        auto existing = read_file(hook);
        if (!existing) continue;
        if (!is_our_hook(*existing)) {
            line += std::string(line.empty() ? "" : ", ") + name + " kept (not lockstep's)";
        } else if (fs::remove(hook, ec)) {
            line += std::string(line.empty() ? "" : ", ") + name + " removed";
        }
    }
    if (!line.empty()) std::printf("  hooks: %s\n", line.c_str());
}

// Read config.json as ordered JSON (preserving key order for rewriting). On a
// missing file returns an empty object; on a parse error returns nullopt.
std::optional<nlohmann::ordered_json> read_config_json() {
    auto raw = read_file(config_path());
    if (!raw) return nlohmann::ordered_json::object();
    auto j = nlohmann::ordered_json::parse(*raw, nullptr, /*exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) return std::nullopt;
    return j;
}

}  // namespace

int cmd_install() {
    auto dir = self_dir();
    if (!dir || !fs::exists(*dir / "lockstepd")) {
        std::fprintf(stderr,
                     "lockstep: can't find lockstepd next to this binary — run install "
                     "from the build output (e.g. ./build/bin/lockstep install)\n");
        return 1;
    }
    // Two daemons would fight over the socket (the newer one steals it), so
    // refuse while one runs outside the service manager.
    if (daemon_up() && !service_loaded()) {
        std::fprintf(stderr,
                     "lockstep: a lockstepd is already running outside the service manager.\n"
                     "  stop it first (pkill lockstepd), then re-run install\n");
        return 1;
    }

    int failures = 0;

    // 1. ~/.local/bin links to this build.
    std::printf("binaries\n");
    for (const char* name : kBinaries) {
        fs::path link = bin_dir() / name, target = *dir / name;
        std::error_code ec;
        fs::create_directories(bin_dir(), ec);
        auto st = fs::symlink_status(link, ec);
        if (fs::exists(st) && !fs::is_symlink(st)) {
            std::printf("  ! %s exists and isn't a symlink; left alone\n", pretty(link).c_str());
            ++failures;
            continue;
        }
        fs::remove(link, ec);
        fs::create_symlink(target, link, ec);
        if (ec) {
            std::printf("  ! %s: %s\n", pretty(link).c_str(), ec.message().c_str());
            ++failures;
        } else {
            std::printf("  %s -> %s\n", pretty(link).c_str(), pretty(target).c_str());
        }
    }

    // 2. Hooks in every watched repo.
    std::printf("hooks\n");
    std::string cfg_err;
    Config cfg = load_config(&cfg_err);
    if (!cfg_err.empty()) {
        std::printf("  ! %s\n", cfg_err.c_str());
        ++failures;
    } else if (cfg.repos.empty()) {
        std::printf("  (no repos watched — add them to %s, then re-run install)\n",
                    pretty(config_path()).c_str());
    }
    for (const auto& repo : cfg.repos) {
        auto hdir = hooks_dir(repo);
        if (!hdir) {
            std::printf("  ! %s: not a git work tree\n", pretty(repo).c_str());
            ++failures;
            continue;
        }
        std::string line;
        for (const char* name : kHookNames) {
            fs::path hook = *hdir / name;
            auto existing = read_file(hook);
            std::string state;
            if (existing && !is_our_hook(*existing)) {
                state = "kept existing (not lockstep's)";
            } else if (existing && *existing == kHookScript) {
                state = "up to date";
            } else if (write_file(hook, kHookScript,
                                  fs::perms::owner_all | fs::perms::group_read |
                                      fs::perms::group_exec | fs::perms::others_read |
                                      fs::perms::others_exec)) {
                state = existing ? "updated" : "installed";
            } else {
                state = "FAILED to write";
                ++failures;
            }
            line += std::string(line.empty() ? "" : ", ") + name + " " + state;
        }
        std::printf("  %-40s %s\n", pretty(repo).c_str(), line.c_str());
    }

    // 3. Service.
    std::printf("service\n");
    if (!write_file(service_file(), service_contents(),
                    fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read |
                        fs::perms::others_read)) {
        std::printf("  ! could not write %s\n", pretty(service_file()).c_str());
        return 1;
    }
    std::printf("  wrote %s\n", pretty(service_file()).c_str());
    std::string err;
    if (!service_start(&err)) {
        std::printf("  ! %s\n", err.c_str());
        return 1;
    }
    if (!wait_for_daemon(50)) {
        std::printf("  ! service started but the daemon isn't answering (%s)\n", kLogsHint);
        return 1;
    }
    std::printf("  daemon running (%s)\n", kLogsHint);
    std::printf("  %s\n", kRestartHint);

    // 4. Tray (optional; only if the Qt build produced it).
    fs::path tray_bin = *dir / "lockstep-tray";
    std::printf("tray\n");
    if (!fs::exists(tray_bin)) {
        std::printf("  (not built — reconfigure with "
                    "-DCMAKE_PREFIX_PATH=\"$(brew --prefix qt)\" to enable the menubar app)\n");
    } else {
#ifdef __APPLE__
        link_binary(*dir, "lockstep-tray");
        if (!write_file(tray_service_file(), tray_service_contents(),
                        fs::perms::owner_read | fs::perms::owner_write |
                            fs::perms::group_read | fs::perms::others_read)) {
            std::printf("  ! could not write %s\n", pretty(tray_service_file()).c_str());
            ++failures;
        } else {
            std::printf("  wrote %s\n", pretty(tray_service_file()).c_str());
            std::string terr;
            if (!tray_start(&terr)) {
                std::printf("  ! %s\n", terr.c_str());
                ++failures;
            } else {
                std::printf("  menubar app running (logs: ~/Library/Logs/lockstep-tray.log)\n");
            }
        }
#else
        link_binary(*dir, "lockstep-tray");
        std::printf("  built, but autostart isn't managed on Linux — launch "
                    "lockstep-tray from your desktop session\n");
#endif
    }

    // The hooks fall back to ~/.local/bin, but interactive use wants it on PATH.
    const char* path = std::getenv("PATH");
    if (!path || (":" + std::string(path) + ":").find(":" + bin_dir().string() + ":") ==
                     std::string::npos)
        std::printf("note: %s isn't on your PATH\n", pretty(bin_dir()).c_str());

    return failures == 0 ? 0 : 1;
}

int cmd_uninstall() {
    std::printf("service\n");
    service_stop();
    std::error_code ec;
    if (fs::remove(service_file(), ec))
        std::printf("  stopped and removed %s\n", pretty(service_file()).c_str());
    else
        std::printf("  (no %s)\n", pretty(service_file()).c_str());
#ifdef __APPLE__
    tray_stop();
    if (fs::remove(tray_service_file(), ec))
        std::printf("  stopped and removed %s\n", pretty(tray_service_file()).c_str());
#else
    run({"systemctl", "--user", "daemon-reload"});
#endif

    std::printf("hooks\n");
    Config cfg = load_config();
    for (const auto& repo : cfg.repos) {
        auto hdir = hooks_dir(repo);
        if (!hdir) continue;
        std::string line;
        for (const char* name : kHookNames) {
            fs::path hook = *hdir / name;
            auto existing = read_file(hook);
            std::string state = !existing               ? "none"
                                : !is_our_hook(*existing) ? "kept (not lockstep's)"
                                : fs::remove(hook, ec)    ? "removed"
                                                          : "FAILED to remove";
            line += std::string(line.empty() ? "" : ", ") + name + " " + state;
        }
        std::printf("  %-40s %s\n", pretty(repo).c_str(), line.c_str());
    }

    std::printf("binaries\n");
    for (const char* name : {"lockstep", "lockstepd", "lockstep-tray"}) {
        fs::path link = bin_dir() / name;
        if (fs::is_symlink(fs::symlink_status(link, ec)) && fs::remove(link, ec))
            std::printf("  removed %s\n", pretty(link).c_str());
    }
    std::printf("config and key left in place: %s\n",
                pretty(fs::path(config_path()).parent_path()).c_str());
    return 0;
}

int cmd_add(const std::string& arg) {
    // Resolve to the repo root so adding a subdirectory adds the whole repo.
    CmdResult top = run({"git", "-C", arg, "rev-parse", "--show-toplevel"});
    if (!ok(top)) {
        std::fprintf(stderr, "lockstep: not a git work tree: %s\n", arg.c_str());
        return 1;
    }
    std::string repo = trim(top.out);

    auto j = read_config_json();
    if (!j) {
        std::fprintf(stderr, "lockstep: config isn't valid JSON: %s\n",
                     pretty(config_path()).c_str());
        return 1;
    }
    if (!j->contains("repos") || !(*j)["repos"].is_array())
        (*j)["repos"] = nlohmann::ordered_json::array();

    for (const auto& e : (*j)["repos"]) {
        std::string p = entry_path(e);
        if (!p.empty() && same_path(p, repo)) {
            std::printf("already watched: %s\n", pretty(repo).c_str());
            install_hooks_report(repo);  // ensure hooks are present anyway
            return 0;
        }
    }

    (*j)["repos"].push_back(repo);
    if (!write_file(config_path(), j->dump(2) + "\n", config_perms())) {
        std::fprintf(stderr, "lockstep: could not write %s\n",
                     pretty(config_path()).c_str());
        return 1;
    }
    std::printf("added %s\n  to %s\n", pretty(repo).c_str(),
                pretty(config_path()).c_str());
    install_hooks_report(repo);
    std::printf("the daemon picks it up on its next sync.\n");
    return 0;
}

int cmd_remove(const std::string& arg) {
    // Accept a path (resolved to its repo root) or a bare repo name.
    std::string target_path, target_name;
    if (CmdResult top = run({"git", "-C", arg, "rev-parse", "--show-toplevel"}); ok(top)) {
        target_path = trim(top.out);
        target_name = repo_name(target_path);
    } else {
        target_name = arg;  // bare name, e.g. "uw-core"
    }

    auto j = read_config_json();
    if (!j) {
        std::fprintf(stderr, "lockstep: config isn't valid JSON: %s\n",
                     pretty(config_path()).c_str());
        return 1;
    }
    if (!j->contains("repos") || !(*j)["repos"].is_array()) {
        std::fprintf(stderr, "lockstep: not watched: %s\n", arg.c_str());
        return 1;
    }

    nlohmann::ordered_json kept = nlohmann::ordered_json::array();
    std::vector<std::string> removed;
    for (const auto& e : (*j)["repos"]) {
        std::string p = entry_path(e);
        bool match = !p.empty() &&
                     ((!target_path.empty() && same_path(p, target_path)) ||
                      repo_name(expand_home(p)) == target_name);
        if (match)
            removed.push_back(expand_home(p));
        else
            kept.push_back(e);
    }

    if (removed.empty()) {
        std::fprintf(stderr, "lockstep: not watched: %s\n", arg.c_str());
        return 1;
    }
    (*j)["repos"] = std::move(kept);
    if (!write_file(config_path(), j->dump(2) + "\n", config_perms())) {
        std::fprintf(stderr, "lockstep: could not write %s\n",
                     pretty(config_path()).c_str());
        return 1;
    }
    for (const auto& r : removed) {
        std::printf("removed %s from config\n", pretty(r).c_str());
        remove_hooks_report(r);
    }
    std::printf("the daemon drops it on its next sync.\n");
    return 0;
}

}  // namespace lockstep::cli
