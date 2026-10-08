// lockstepd — the per-machine daemon.
//
// This first slice is intentionally thin: it opens the Unix socket and answers
// verdict/status/ping requests with a hardcoded "clear" verdict. The real
// machinery (repo watching, GitHub rendezvous, the other machine's cached
// state) hangs off this same socket surface and lands in later slices.

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <memory>
#include <string>

#include <sys/socket.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include "blob.h"
#include "config.h"
#include "crypto.h"
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

// Decide whether committing here is clear, given the other machine(s)' state.
// Policy: fail OPEN (clear, with a note) when the rendezvous/key isn't set up or
// can't be read — a dev tool must not block real work over its own plumbing.
// Block only on a definite "other machine has pending work" signal. `repo_filter`
// (optional) restricts the check to one repo name (the committing repo).
json decide_verdict(const lockstep::Config& cfg, const std::string& repo_filter) {
    std::string key_path = resolve_key_path(cfg);

    auto rv = make_rendezvous(cfg);
    if (!rv) {
        return {{"ok", true}, {"clear", true},
                {"message", "clear (rendezvous not configured)"}};
    }
    std::string kerr;
    auto key = lockstep::crypto::load_key(key_path, &kerr);
    if (!key) {
        return {{"ok", true}, {"clear", true},
                {"message", "clear (no shared key: " + kerr + ")"}};
    }

    publish_self(cfg, *key, *rv);  // keep our slot fresh

    std::string ferr;
    auto others = rv->fetch_others(lockstep::machine_id(cfg), &ferr);
    if (!others) {
        return {{"ok", true}, {"clear", true},
                {"message", "clear (rendezvous unreadable: " + ferr + ")"}};
    }

    json blockers = json::array();
    int undecryptable = 0;
    for (const auto& [machine, enc] : *others) {
        auto plain = lockstep::crypto::decrypt(*key, enc);
        if (!plain) {
            ++undecryptable;
            log_line("warn", "could not decrypt blob from " + machine +
                                 " (wrong key or tampered)");
            continue;
        }
        auto parsed = nlohmann::json::parse(*plain, nullptr, false);
        if (parsed.is_discarded()) continue;
        auto b = lockstep::blob_from_json(parsed);
        if (!b) continue;

        for (const auto& r : b->repos) {
            if (!repo_filter.empty() && r.name != repo_filter) continue;
            if (r.clean) continue;
            std::string what;
            if (r.dirty > 0) what += std::to_string(r.dirty) + " uncommitted";
            if (r.ahead > 0) {
                if (!what.empty()) what += ", ";
                what += std::to_string(r.ahead) + " unpushed";
            }
            blockers.push_back(
                {{"machine", b->machine}, {"repo", r.name}, {"branch", r.branch},
                 {"detail", what}, {"as_of", human_age(b->timestamp)}});
        }
    }

    if (blockers.empty()) {
        std::string msg = "clear (other machine has no pending work)";
        if (undecryptable > 0)
            msg = "clear, BUT could not decrypt " + std::to_string(undecryptable) +
                  " blob(s) — key mismatch between machines? guard is not protecting"
                  " those.";
        return {{"ok", true}, {"clear", true}, {"message", msg}};
    }
    // Human one-liner for the first blocker; full list in `blockers`.
    const auto& b0 = blockers.front();
    std::string msg = "\xe2\x9b\x94 " + b0.value("machine", "?") + " has " +
                      b0.value("detail", "pending work") + " on " +
                      b0.value("repo", "?") + " (" + b0.value("as_of", "") +
                      "). Pull or resolve before committing here.";
    return {{"ok", true}, {"clear", false}, {"message", msg},
            {"blockers", std::move(blockers)}};
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

    while (!g_stop) {
        int client_fd = ::accept(listen_fd, nullptr, nullptr);
        if (client_fd < 0) {
            if (errno == EINTR) continue;  // interrupted by SIGTERM/SIGINT
            log_line("warn", std::string("accept: ") + std::strerror(errno));
            continue;
        }
        serve_one(client_fd);
        ::close(client_fd);
    }

    log_line("info", "shutting down");
    ::close(listen_fd);
    ::unlink(path.c_str());
    return 0;
}
