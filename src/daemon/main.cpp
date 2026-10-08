// lockstepd — the per-machine daemon.
//
// A single-threaded poll() loop does two things: serve verdict/status/ping over
// the Unix socket, and on a timer run a background "tick" that publishes this
// machine's state to the rendezvous and fetches + decrypts the other machines'.
// The tick's result is cached, so verdict/status answer from memory with no
// network — commits don't pay a GitHub round-trip. Single-threaded on purpose:
// no mutex, and no fork()-in-a-thread hazard from shelling out to git.

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include "blob.h"
#include "config.h"
#include "crypto.h"
#include "deps.h"
#include "git.h"
#include "ipc.h"
#include "paths.h"
#include "rendezvous.h"

using nlohmann::json;

namespace {

volatile std::sig_atomic_t g_stop = 0;

void on_signal(int) { g_stop = 1; }

// Install `handler` for `sig` WITHOUT SA_RESTART, so a blocking accept() is
// interrupted (returns EINTR) rather than auto-restarted. std::signal uses BSD
// restart semantics on macOS, which would otherwise swallow the signal and hang
// the accept loop.
void install_handler(int sig, void (*handler)(int)) {
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;  // no SA_RESTART
    ::sigaction(sig, &sa, nullptr);
}

void log_line(const char* level, const std::string& msg) {
    std::time_t t = std::time(nullptr);
    char ts[32];
    std::strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", std::localtime(&t));
    std::fprintf(stderr, "%s [%s] %s\n", ts, level, msg.c_str());
}

// Inspect every configured repo and return {repos: [...], summary line}.
struct LocalScan {
    json repos = json::array();
    std::string message;
};

LocalScan scan_local() {
    LocalScan scan;
    std::string cfg_err;
    lockstep::Config cfg = lockstep::load_config(&cfg_err);

    if (!cfg_err.empty()) {
        scan.message = cfg_err;
        return scan;
    }
    if (cfg.repos.empty()) {
        scan.message = "no repos watched — add paths to " + lockstep::config_path();
        return scan;
    }

    int pending = 0;      // real local work: dirty or ahead of upstream
    int unreadable = 0;   // path isn't a readable git work tree
    for (const auto& path : cfg.repos) {
        lockstep::RepoState s = lockstep::inspect(path);
        json entry = {{"path", s.path},
                      {"is_repo", s.is_repo},
                      {"branch", s.branch},
                      {"dirty", s.dirty},
                      {"has_upstream", s.has_upstream},
                      {"ahead", s.ahead},
                      {"behind", s.behind}};
        if (!s.error.empty()) entry["error"] = s.error;
        entry["clean"] = lockstep::is_clean(s);

        if (!s.error.empty() || !s.is_repo) {
            ++unreadable;
        } else if (s.dirty > 0 || (s.has_upstream && s.ahead > 0)) {
            ++pending;
        }
        scan.repos.push_back(std::move(entry));
    }

    scan.message = std::to_string(cfg.repos.size()) + " repo(s) watched, " +
                   std::to_string(pending) + " with pending local work";
    if (unreadable > 0)
        scan.message += ", " + std::to_string(unreadable) + " unreadable";
    return scan;
}

std::string env_or_empty(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

// Shared key path: $LOCKSTEP_KEY > config key_path > sibling "key" of config.json.
std::string resolve_key_path(const lockstep::Config& cfg) {
    if (std::string e = env_or_empty("LOCKSTEP_KEY"); !e.empty()) return e;
    if (!cfg.key_path.empty()) return cfg.key_path;
    std::filesystem::path cfgp = lockstep::config_path();
    return (cfgp.parent_path() / "key").string();
}

std::string resolve_rendezvous_dir(const lockstep::Config& cfg) {
    if (std::string e = env_or_empty("LOCKSTEP_RENDEZVOUS_DIR"); !e.empty()) return e;
    return cfg.rendezvous_dir;
}

std::string resolve_rendezvous_repo(const lockstep::Config& cfg) {
    if (std::string e = env_or_empty("LOCKSTEP_RENDEZVOUS_REPO"); !e.empty()) return e;
    return cfg.rendezvous_repo;
}

// Pick the backend: a git-clone rendezvous repo wins over the local file dir
// (the latter is for tests/interim). Returns null when neither is configured.
std::unique_ptr<lockstep::Rendezvous> make_rendezvous(const lockstep::Config& cfg) {
    if (std::string repo = resolve_rendezvous_repo(cfg); !repo.empty())
        return std::make_unique<lockstep::GitHubRendezvous>(repo);
    if (std::string dir = resolve_rendezvous_dir(cfg); !dir.empty())
        return std::make_unique<lockstep::FileRendezvous>(dir);
    return nullptr;
}

std::string human_age(int64_t ts) {
    int64_t secs = static_cast<int64_t>(std::time(nullptr)) - ts;
    if (secs < 0) secs = 0;
    if (secs < 90) return std::to_string(secs) + "s ago";
    if (secs < 90 * 60) return std::to_string(secs / 60) + "m ago";
    if (secs < 48 * 3600) return std::to_string(secs / 3600) + "h ago";
    return std::to_string(secs / 86400) + "d ago";
}

// Publish this machine's current state to the rendezvous (best effort).
void publish_self(const lockstep::Config& cfg, const lockstep::crypto::Key& key,
                  lockstep::Rendezvous& rv) {
    std::string self = lockstep::machine_id(cfg);
    std::string plain = lockstep::to_json(lockstep::gather(cfg)).dump();
    auto enc = lockstep::crypto::encrypt(key, plain);
    if (!enc) return;
    std::string err;
    if (!rv.publish(self, *enc, &err))
        log_line("warn", "publish failed: " + err);
}

// Human description of a repo's pending work, e.g. "3 uncommitted, 1 unpushed".
std::string pending_detail(const lockstep::RepoBrief& r) {
    std::string what;
    if (r.dirty > 0) what += std::to_string(r.dirty) + " uncommitted";
    if (r.ahead > 0) {
        if (!what.empty()) what += ", ";
        what += std::to_string(r.ahead) + " unpushed";
    }
    return what;
}

// Local warnings for committing in `repo`: its submodule pins of watched repos
// that don't match the standalone clone's HEAD.
json pin_warnings(const lockstep::Config& cfg, const std::string& repo) {
    json warnings = json::array();
    auto path = lockstep::find_repo_path(cfg, repo);
    if (!path) return warnings;
    for (const auto& d : lockstep::pin_drift(cfg, *path)) {
        std::string msg = repo + " pins " + d.dep_name + " (" + d.sub_path + ") ";
        if (d.unknown) {
            msg += "at a commit your " + d.dep_name + " clone doesn't have — pull " +
                   d.dep_name + "?";
        } else if (d.pin_only == 0) {
            msg += std::to_string(d.clone_only) + " commit(s) behind your " +
                   d.dep_name + " clone — update the submodule?";
        } else if (d.clone_only == 0) {
            msg += std::to_string(d.pin_only) + " commit(s) ahead of your " +
                   d.dep_name + " clone — pull " + d.dep_name + "?";
        } else {
            msg += "on a commit that has diverged from your " + d.dep_name + " clone";
        }
        warnings.push_back({{"kind", "pin"}, {"repo", d.dep_name}, {"message", msg}});
    }
    return warnings;
}

// Cached result of the last background tick, so verdict/status answer from
// memory with no network. Single-threaded daemon, so no locking is needed.
struct RendezvousCache {
    std::vector<lockstep::MachineBlob> others;  // other machines' decrypted state
    int undecryptable = 0;                      // blobs we couldn't decrypt (key mismatch?)
    bool configured = false;                    // rendezvous + key both present
    bool ever = false;                          // at least one tick has run
    std::string status;                         // "" on clean success; else a reason
    int64_t last_ok = 0;                        // unix secs of last successful fetch
};
RendezvousCache g_cache;
int g_tick_seconds = 30;

int resolve_tick_seconds(const lockstep::Config& cfg) {
    int s = cfg.tick_seconds;
    if (std::string e = env_or_empty("LOCKSTEP_TICK_SECONDS"); !e.empty())
        s = std::atoi(e.c_str());
    if (s <= 0) s = 30;      // default
    if (s < 5) s = 5;        // floor: don't hammer the remote
    if (s > 3600) s = 3600;  // ceiling
    return s;
}

// Refresh g_cache from the rendezvous: publish our state, fetch + decrypt the
// others. On a config problem (no rendezvous/key) we clear the cached others —
// there's genuinely nothing to compare against. On a transient fetch error we
// KEEP the last-known others (stale state beats silently dropping the guard);
// their age shows through the per-machine timestamps.
void do_tick() {
    lockstep::Config cfg = lockstep::load_config();
    g_tick_seconds = resolve_tick_seconds(cfg);
    g_cache.ever = true;

    auto rv = make_rendezvous(cfg);
    if (!rv) {
        g_cache.configured = false;
        g_cache.status = "rendezvous not configured";
        g_cache.others.clear();
        g_cache.undecryptable = 0;
        return;
    }
    std::string kerr;
    auto key = lockstep::crypto::load_key(resolve_key_path(cfg), &kerr);
    if (!key) {
        g_cache.configured = false;
        g_cache.status = "no shared key: " + kerr;
        g_cache.others.clear();
        g_cache.undecryptable = 0;
        return;
    }
    g_cache.configured = true;

    publish_self(cfg, *key, *rv);

    std::string ferr;
    auto others = rv->fetch_others(lockstep::machine_id(cfg), &ferr);
    if (!others) {
        g_cache.status = "rendezvous unreadable: " + ferr;  // keep stale others
        return;
    }

    // Warn once per machine per run on a decrypt failure, so a persistent key
    // mismatch doesn't spam the log every tick.
    static std::set<std::string> warned_decrypt;
    std::vector<lockstep::MachineBlob> fresh;
    int undec = 0;
    for (auto& [machine, enc] : *others) {
        auto plain = lockstep::crypto::decrypt(*key, enc);
        if (!plain) {
            ++undec;
            if (warned_decrypt.insert(machine).second)
                log_line("warn", "could not decrypt blob from " + machine +
                                     " (wrong key or tampered)");
            continue;
        }
        warned_decrypt.erase(machine);
        auto parsed = nlohmann::json::parse(*plain, nullptr, false);
        if (parsed.is_discarded()) continue;
        auto b = lockstep::blob_from_json(parsed);
        if (!b) continue;
        fresh.push_back(std::move(*b));
    }
    g_cache.others = std::move(fresh);
    g_cache.undecryptable = undec;
    g_cache.status = "";
    g_cache.last_ok = static_cast<int64_t>(std::time(nullptr));
}

// Decide whether committing here is clear, reading the cached other-machine
// state (no network — the tick keeps it fresh). Policy: fail OPEN (clear, with a
// note) when the rendezvous/key isn't set up — a dev tool must not block real
// work over its own plumbing. Block only on a definite "other machine has
// pending work" signal. `repo_filter` (optional) restricts the check to one repo
// name (the committing repo).
//
// Dependencies of the committing repo never block; they produce `warnings`
// (pending work on a dependency elsewhere, or local submodule pin drift), which
// the hook turns into a "commit anyway?" prompt.
json decide_verdict(const lockstep::Config& cfg, const std::string& repo_filter) {
    // Pin drift is local git state — compute it fresh each call, not on the tick.
    json warnings = repo_filter.empty() ? json::array() : pin_warnings(cfg, repo_filter);
    std::vector<std::string> deps;
    if (!repo_filter.empty()) deps = lockstep::dependencies(cfg, repo_filter);
    auto is_dep = [&](const std::string& n) {
        return std::find(deps.begin(), deps.end(), n) != deps.end();
    };
    auto reply = [&](bool clear, const std::string& msg) {
        return json{{"ok", true}, {"clear", clear}, {"message", msg},
                    {"warnings", warnings}};
    };

    if (!g_cache.configured) {
        std::string why = g_cache.status.empty() ? "rendezvous not configured"
                                                 : g_cache.status;
        return reply(true, "clear (" + why + ")");
    }

    json blockers = json::array();
    json notes = json::array();
    for (const auto& b : g_cache.others) {
        std::vector<std::string> unseen = deps;  // deps this machine doesn't report
        for (const auto& r : b.repos) {
            bool dep = is_dep(r.name);
            if (auto u = std::find(unseen.begin(), unseen.end(), r.name); u != unseen.end())
                unseen.erase(u);
            if (!dep && !repo_filter.empty() && r.name != repo_filter) continue;
            if (r.clean) continue;
            json item = {{"machine", b.machine}, {"repo", r.name}, {"branch", r.branch},
                         {"detail", pending_detail(r)}, {"as_of", human_age(b.timestamp)}};
            if (dep) {
                item["kind"] = "dependency";
                item["message"] = b.machine + " has " + pending_detail(r) + " on " +
                                  r.name + " (" + human_age(b.timestamp) + "), which " +
                                  repo_filter + " depends on";
                warnings.push_back(std::move(item));
            } else {
                blockers.push_back(std::move(item));
            }
        }
        for (const auto& u : unseen)
            notes.push_back(u + " isn't watched on " + b.machine +
                            ", so its state there is unknown");
    }

    json out;
    if (blockers.empty()) {
        std::string msg = warnings.empty()
                              ? "clear (other machine has no pending work)"
                              : "not blocked, but:";
        if (g_cache.undecryptable > 0)
            msg = "clear, BUT could not decrypt " + std::to_string(g_cache.undecryptable) +
                  " blob(s) — key mismatch between machines? guard is not protecting"
                  " those.";
        out = reply(true, msg);
    } else {
        // Human one-liner for the first blocker; full list in `blockers`.
        const auto& b0 = blockers.front();
        std::string msg = "\xe2\x9b\x94 " + b0.value("machine", "?") + " has " +
                          b0.value("detail", "pending work") + " on " +
                          b0.value("repo", "?") + " (" + b0.value("as_of", "") +
                          "). Pull or resolve before committing here.";
        out = reply(false, msg);
        out["blockers"] = std::move(blockers);
    }
    // Surface a degraded sync (stale data) without blocking.
    if (!g_cache.status.empty())
        notes.push_back("sync issue: " + g_cache.status + " (showing last known state)");
    if (!notes.empty()) out["notes"] = std::move(notes);
    return out;
}

// Build the reply for one request.
json handle(const json& req) {
    std::string cmd = req.value("cmd", "");

    if (cmd == lockstep::ipc::kCmdPing) {
        return {{"ok", true}, {"pong", true}};
    }
    if (cmd == lockstep::ipc::kCmdVerdict) {
        // Verdict is about the OTHER machine's pending work. The committing repo
        // (if the hook sends it) narrows the check to that repo.
        lockstep::Config cfg = lockstep::load_config();
        return decide_verdict(cfg, req.value("repo", ""));
    }
    if (cmd == lockstep::ipc::kCmdStatus) {
        LocalScan scan = scan_local();
        return {{"ok", true},
                {"clear", true},
                {"repos", std::move(scan.repos)},
                {"message", scan.message}};
    }
    return {{"ok", false}, {"error", "unknown command: " + cmd}};
}

void serve_one(int client_fd) {
    auto req = lockstep::ipc::recv_message(client_fd);
    if (!req) {
        lockstep::ipc::send_message(client_fd,
                                    {{"ok", false}, {"error", "bad request"}});
        return;
    }
    lockstep::ipc::send_message(client_fd, handle(*req));
}

}  // namespace

int main() {
    // Line-buffer stderr so supervisors capture logs promptly.
    std::setvbuf(stderr, nullptr, _IOLBF, 0);

    if (!lockstep::crypto::init()) {
        log_line("error", "libsodium init failed");
        return 1;
    }

    // Never let a git credential prompt hang the daemon: fail fast instead.
    ::setenv("GIT_TERMINAL_PROMPT", "0", 1);

    install_handler(SIGTERM, on_signal);
    install_handler(SIGINT, on_signal);
    std::signal(SIGPIPE, SIG_IGN);  // a client hanging up mid-reply must not kill us

    const std::string path = lockstep::socket_path();
    int listen_fd = lockstep::ipc::listen_unix(path);
    if (listen_fd < 0) {
        log_line("error", std::string("failed to listen on ") + path + ": " +
                              std::strerror(errno));
        return 1;
    }
    log_line("info", "lockstepd listening on " + path);

    do_tick();  // warm the cache before serving the first request
    log_line("info", "background sync every " + std::to_string(g_tick_seconds) + "s");
    int64_t next_tick = static_cast<int64_t>(std::time(nullptr)) + g_tick_seconds;

    while (!g_stop) {
        int64_t now = static_cast<int64_t>(std::time(nullptr));
        int timeout_ms = next_tick <= now
                             ? 0
                             : static_cast<int>((next_tick - now) * 1000);
        struct pollfd pfd{listen_fd, POLLIN, 0};
        int n = ::poll(&pfd, 1, timeout_ms);
        if (g_stop) break;
        if (n < 0) {
            if (errno == EINTR) continue;  // interrupted by SIGTERM/SIGINT
            log_line("warn", std::string("poll: ") + std::strerror(errno));
            continue;
        }
        if (n > 0 && (pfd.revents & POLLIN)) {
            int client_fd = ::accept(listen_fd, nullptr, nullptr);
            if (client_fd >= 0) {
                serve_one(client_fd);
                ::close(client_fd);
            } else if (errno != EINTR) {
                log_line("warn", std::string("accept: ") + std::strerror(errno));
            }
        }
        // Due for a tick? (poll timed out, or a request arrived past the deadline.)
        if (static_cast<int64_t>(std::time(nullptr)) >= next_tick) {
            do_tick();
            next_tick = static_cast<int64_t>(std::time(nullptr)) + g_tick_seconds;
        }
    }

    log_line("info", "shutting down");
    ::close(listen_fd);
    ::unlink(path.c_str());
    return 0;
}
