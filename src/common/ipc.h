#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

// Thin, blocking Unix-domain-socket transport shared by the daemon and its
// clients (CLI + git hooks). Wire format: one line of JSON per message,
// terminated by '\n'. Requests carry a "cmd"; responses carry an "ok" flag.
namespace lockstep::ipc {

// Command names understood by the daemon.
inline constexpr const char* kCmdVerdict = "verdict";  // "am I clear to commit?"
inline constexpr const char* kCmdStatus = "status";    // human-facing summary
inline constexpr const char* kCmdPing = "ping";        // liveness check

// --- Server side -----------------------------------------------------------

// Create, bind, and listen on a Unix socket at `path`, removing any stale
// socket file first. Returns the listening fd, or -1 (with errno set).
int listen_unix(const std::string& path, int backlog = 16);

// --- Client side -----------------------------------------------------------

// Connect to the daemon socket at `path`. Returns a connected fd, or -1
// (with errno set) — e.g. ENOENT/ECONNREFUSED when the daemon isn't running.
int connect_unix(const std::string& path);

// Send one JSON message (adds the trailing '\n'). Returns true on success.
bool send_message(int fd, const nlohmann::json& msg);

// Read one '\n'-terminated JSON message. Returns nullopt on EOF/parse error.
std::optional<nlohmann::json> recv_message(int fd);

// Convenience: connect, send `req`, read one reply, close. On transport
// failure returns nullopt and, if `err` is non-null, sets a human message.
std::optional<nlohmann::json> request(const std::string& path,
                                      const nlohmann::json& req,
                                      std::string* err = nullptr);

}  // namespace lockstep::ipc
