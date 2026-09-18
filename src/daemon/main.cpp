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

// Build the reply for one request. Hardcoded "clear" for now.
json handle(const json& req) {
    std::string cmd = req.value("cmd", "");

    if (cmd == lockstep::ipc::kCmdPing) {
        return {{"ok", true}, {"pong", true}};
    }
    if (cmd == lockstep::ipc::kCmdVerdict) {
        // TODO(slice 2+): consult cached other-machine state here.
        return {{"ok", true}, {"clear", true}, {"message", "clear (stub)"}};
    }
    if (cmd == lockstep::ipc::kCmdStatus) {
        return {{"ok", true},
                {"clear", true},
                {"repos", json::array()},
                {"message", "no repos watched yet (stub daemon)"}};
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
