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
#include <string>

#include <sys/socket.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include "config.h"
#include "git.h"
#include "ipc.h"
#include "paths.h"

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

// Build the reply for one request.
json handle(const json& req) {
    std::string cmd = req.value("cmd", "");

    if (cmd == lockstep::ipc::kCmdPing) {
        return {{"ok", true}, {"pong", true}};
    }
    if (cmd == lockstep::ipc::kCmdVerdict) {
        // Verdict is about the OTHER machine's pending work, which requires the
        // GitHub rendezvous (slice 3). Until that exists there's no remote state
        // to block on, so we stay clear. Local dirty state is the user's own and
        // never blocks their commit.
        return {{"ok", true},
                {"clear", true},
                {"message", "clear (no other-machine state yet)"}};
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
