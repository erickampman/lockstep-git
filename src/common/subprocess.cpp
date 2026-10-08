#include "subprocess.h"

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>

namespace lockstep {

CmdResult run(const std::vector<std::string>& argv) {
    CmdResult result;
    if (argv.empty()) return result;

    int pipe_fd[2];
    if (::pipe(pipe_fd) < 0) return result;

    pid_t pid = ::fork();
    if (pid < 0) {
        ::close(pipe_fd[0]);
        ::close(pipe_fd[1]);
        return result;
    }

    if (pid == 0) {
        // Child: stdout -> pipe write end, stderr -> /dev/null.
        ::dup2(pipe_fd[1], STDOUT_FILENO);
        int devnull = ::open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            ::dup2(devnull, STDERR_FILENO);
            ::close(devnull);
        }
        ::close(pipe_fd[0]);
        ::close(pipe_fd[1]);

        std::vector<char*> cargv;
        cargv.reserve(argv.size() + 1);
        for (const auto& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
        cargv.push_back(nullptr);

        ::execvp(cargv[0], cargv.data());
        // execvp only returns on failure.
        ::_exit(127);
    }

    // Parent: read all of stdout, then reap.
    ::close(pipe_fd[1]);
    char buf[4096];
    for (;;) {
        ssize_t n = ::read(pipe_fd[0], buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;
        result.out.append(buf, static_cast<size_t>(n));
    }
    ::close(pipe_fd[0]);

    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    result.spawned = true;
    result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return result;
}

}  // namespace lockstep
