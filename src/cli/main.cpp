// lockstep — the CLI. A thin client of the daemon socket.
//
//   lockstep status   human-facing summary of both machines
//   lockstep verdict  "am I clear to commit?" — exit 0 clear, 1 blocked
//   lockstep ping     liveness check
//
// Everything here is IPC + formatting; the daemon holds the state.

#include <cstdio>
#include <cstring>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "ipc.h"
#include "paths.h"

using nlohmann::json;

namespace {

int usage(const char* argv0) {
    std::fprintf(stderr,
                 "usage: %s <command>\n"
                 "  status    summary of watched repos / both machines\n"
                 "  verdict   exit 0 if clear to commit, 1 if blocked\n"
                 "  ping      check the daemon is running\n",
                 argv0);
    return 2;
}

// Send one command to the daemon, print a transport error if unreachable.
std::optional<json> ask(const char* cmd) {
    std::string err;
    auto reply = lockstep::ipc::request(lockstep::socket_path(),
                                        {{"cmd", cmd}}, &err);
    if (!reply) {
        std::fprintf(stderr, "lockstep: cannot reach daemon: %s\n", err.c_str());
        std::fprintf(stderr, "  (is lockstepd running?)\n");
    }
    return reply;
}

int cmd_ping() {
    auto reply = ask(lockstep::ipc::kCmdPing);
    if (!reply) return 1;
    if (reply->value("ok", false)) {
        std::printf("daemon is up\n");
        return 0;
    }
    std::fprintf(stderr, "daemon replied with error\n");
    return 1;
}

// One-line human summary of a repo's local state.
std::string describe_repo(const json& r) {
    if (r.contains("error")) {
        return r.value("error", "error");
    }
    std::string s;
    int dirty = r.value("dirty", 0);
    s += (dirty == 0) ? "clean" : (std::to_string(dirty) + " dirty");
    if (r.value("has_upstream", false)) {
        int ahead = r.value("ahead", 0);
        int behind = r.value("behind", 0);
        if (ahead == 0 && behind == 0) {
            s += ", up to date";
        } else {
            if (ahead) s += ", ahead " + std::to_string(ahead);
            if (behind) s += ", behind " + std::to_string(behind);
        }
    } else {
        s += ", no upstream";
    }
    return s;
}

int cmd_status() {
    auto reply = ask(lockstep::ipc::kCmdStatus);
    if (!reply) return 1;

    std::printf("%s\n", reply->value("message", "(no message)").c_str());

    if (auto it = reply->find("repos"); it != reply->end() && it->is_array()) {
        for (const auto& r : *it) {
            const char* mark = r.value("clean", false) ? "\xe2\x9c\x93" : "\xe2\x97\x8f";  // ✓ / ●
            std::string branch = r.value("branch", "");
            std::printf("  %s %-40s %-12s %s\n", mark,
                        r.value("path", "?").c_str(),
                        branch.empty() ? "-" : branch.c_str(),
                        describe_repo(r).c_str());
        }
    }
    return reply->value("clear", true) ? 0 : 1;
}

int cmd_verdict() {
    auto reply = ask(lockstep::ipc::kCmdVerdict);
    // Fail open on transport error is a policy decision for a later slice; for
    // now an unreachable daemon is a hard error so it's never silently ignored.
    if (!reply) return 1;
    bool clear = reply->value("clear", false);
    std::string msg = reply->value("message", clear ? "clear" : "blocked");
    if (clear) {
        std::printf("%s\n", msg.c_str());
        return 0;
    }
    std::fprintf(stderr, "%s\n", msg.c_str());
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) return usage(argv[0]);
    std::string cmd = argv[1];

    if (cmd == "ping") return cmd_ping();
    if (cmd == "status") return cmd_status();
    if (cmd == "verdict") return cmd_verdict();
    if (cmd == "-h" || cmd == "--help" || cmd == "help") { usage(argv[0]); return 0; }

    std::fprintf(stderr, "lockstep: unknown command '%s'\n", cmd.c_str());
    return usage(argv[0]);
}
