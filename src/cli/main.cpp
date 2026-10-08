// lockstep — the CLI. A thin client of the daemon socket.
//
//   lockstep status   human-facing summary of both machines
//   lockstep verdict  "am I clear to commit?" — exit 0 clear, 1 blocked
//                     (--warn-exit: exit 3 when clear but with warnings)
//   lockstep ping     liveness check
//
// Everything here is IPC + formatting; the daemon holds the state.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "config.h"
#include "crypto.h"
#include "ipc.h"
#include "paths.h"
#include "subprocess.h"

using nlohmann::json;

namespace {

int usage(const char* argv0) {
    std::fprintf(stderr,
                 "usage: %s <command>\n"
                 "  status    summary of watched repos / both machines\n"
                 "  verdict   exit 0 if clear to commit, 1 if blocked\n"
                 "            --warn-exit: exit 3 if clear but with warnings\n"
                 "  ping      check the daemon is running\n"
                 "  keygen    generate the shared rendezvous key (once per pair)\n",
                 argv0);
    return 2;
}

// Resolve the shared-key path the same way the daemon does:
// $LOCKSTEP_KEY > sibling "key" of the config file.
std::string key_path() {
    if (const char* e = std::getenv("LOCKSTEP_KEY"); e && *e) return e;
    return (std::filesystem::path(lockstep::config_path()).parent_path() / "key")
        .string();
}

// Logical name of the git repo containing the current directory, for narrowing
// the verdict to the committing repo. Empty if we're not in a repo.
std::string current_repo_name() {
    lockstep::CmdResult r = lockstep::run({"git", "rev-parse", "--show-toplevel"});
    if (!r.spawned || r.exit_code != 0) return "";
    std::string top = r.out;
    while (!top.empty() && (top.back() == '\n' || top.back() == '\r')) top.pop_back();
    if (top.empty()) return "";
    return std::filesystem::path(top).filename().string();
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

// Exit code for "clear, but with warnings" under --warn-exit. Without the flag
// warnings exit 0, so older hooks that treat any non-zero as "blocked" keep
// working unchanged.
constexpr int kExitWarn = 3;

int cmd_verdict(bool warn_exit) {
    json req = {{"cmd", lockstep::ipc::kCmdVerdict}};
    if (std::string repo = current_repo_name(); !repo.empty()) req["repo"] = repo;

    std::string err;
    auto reply = lockstep::ipc::request(lockstep::socket_path(), req, &err);
    if (!reply) {
        // An unreachable daemon is a hard error (exit 1) so a missing guard is
        // never silently ignored at the one moment it's supposed to speak up.
        std::fprintf(stderr, "lockstep: cannot reach daemon: %s\n", err.c_str());
        std::fprintf(stderr, "  (is lockstepd running?)\n");
        return 1;
    }
    bool clear = reply->value("clear", false);
    std::string msg = reply->value("message", clear ? "clear" : "blocked");
    if (clear) std::printf("%s\n", msg.c_str());
    else std::fprintf(stderr, "%s\n", msg.c_str());

    bool warned = false;
    if (auto it = reply->find("warnings"); it != reply->end() && it->is_array()) {
        for (const auto& w : *it) {
            std::fprintf(stderr, "\xe2\x9a\xa0 %s\n", w.value("message", "?").c_str());  // ⚠
            warned = true;
        }
    }
    if (auto it = reply->find("notes"); it != reply->end() && it->is_array()) {
        for (const auto& n : *it)
            if (n.is_string()) std::fprintf(stderr, "  note: %s\n", n.get<std::string>().c_str());
    }

    if (!clear) return 1;
    return (warned && warn_exit) ? kExitWarn : 0;
}

int cmd_keygen() {
    if (!lockstep::crypto::init()) {
        std::fprintf(stderr, "lockstep: crypto init failed\n");
        return 1;
    }
    std::string path = key_path();
    if (std::filesystem::exists(path)) {
        std::fprintf(stderr,
                     "lockstep: key already exists at %s\n"
                     "  refusing to overwrite (delete it first to regenerate)\n",
                     path.c_str());
        return 1;
    }
    std::error_code ec;
    std::filesystem::create_directories(
        std::filesystem::path(path).parent_path(), ec);

    std::string err;
    if (!lockstep::crypto::save_key(path, lockstep::crypto::generate_key(), &err)) {
        std::fprintf(stderr, "lockstep: %s\n", err.c_str());
        return 1;
    }
    std::printf("wrote shared key to %s (mode 0600)\n", path.c_str());
    std::printf("copy this file out of band to the other machine's same path.\n");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) return usage(argv[0]);
    std::string cmd = argv[1];

    if (cmd == "ping") return cmd_ping();
    if (cmd == "status") return cmd_status();
    if (cmd == "verdict")
        return cmd_verdict(argc > 2 && std::strcmp(argv[2], "--warn-exit") == 0);
    if (cmd == "keygen") return cmd_keygen();
    if (cmd == "-h" || cmd == "--help" || cmd == "help") { usage(argv[0]); return 0; }

    std::fprintf(stderr, "lockstep: unknown command '%s'\n", cmd.c_str());
    return usage(argv[0]);
}
