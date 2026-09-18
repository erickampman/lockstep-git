#include "ipc.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>

namespace lockstep::ipc {

namespace {

// Fill a sockaddr_un from `path`, guarding against the sun_path length limit
// (104 on macOS, 108 on Linux). Returns false if the path is too long.
bool fill_addr(sockaddr_un& addr, const std::string& path) {
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) {
        errno = ENAMETOOLONG;
        return false;
    }
    std::memcpy(addr.sun_path, path.c_str(), path.size());
    return true;
}

}  // namespace

int listen_unix(const std::string& path, int backlog) {
    sockaddr_un addr;
    if (!fill_addr(addr, path)) return -1;

    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    // Remove any stale socket file from a previous (crashed) run.
    ::unlink(path.c_str());

    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        int e = errno;
        ::close(fd);
        errno = e;
        return -1;
    }
    if (::listen(fd, backlog) < 0) {
        int e = errno;
        ::close(fd);
        errno = e;
        return -1;
    }
    return fd;
}

int connect_unix(const std::string& path) {
    sockaddr_un addr;
    if (!fill_addr(addr, path)) return -1;

    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        int e = errno;
        ::close(fd);
        errno = e;
        return -1;
    }
    return fd;
}

bool send_message(int fd, const nlohmann::json& msg) {
    std::string line = msg.dump();
    line.push_back('\n');
    size_t sent = 0;
    while (sent < line.size()) {
        ssize_t n = ::write(fd, line.data() + sent, line.size() - sent);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

std::optional<nlohmann::json> recv_message(int fd) {
    std::string buf;
    char c;
    for (;;) {
        ssize_t n = ::read(fd, &c, 1);
        if (n < 0) {
            if (errno == EINTR) continue;
            return std::nullopt;
        }
        if (n == 0) {  // EOF before newline
            if (buf.empty()) return std::nullopt;
            break;  // tolerate a final unterminated line
        }
        if (c == '\n') break;
        buf.push_back(c);
    }
    return nlohmann::json::parse(buf, nullptr, /*allow_exceptions=*/false);
}

std::optional<nlohmann::json> request(const std::string& path,
                                      const nlohmann::json& req,
                                      std::string* err) {
    int fd = connect_unix(path);
    if (fd < 0) {
        if (err) *err = std::strerror(errno);
        return std::nullopt;
    }
    struct FdGuard {
        int fd;
        ~FdGuard() { ::close(fd); }
    } guard{fd};

    if (!send_message(fd, req)) {
        if (err) *err = "failed to send request";
        return std::nullopt;
    }
    auto reply = recv_message(fd);
    if (!reply) {
        if (err) *err = "no/invalid reply from daemon";
        return std::nullopt;
    }
    return reply;
}

}  // namespace lockstep::ipc
