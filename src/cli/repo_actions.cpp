#include "repo_actions.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "config.h"
#include "milestones.h"
#include "subprocess.h"

namespace lockstep::cli {

namespace {

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
        s.pop_back();
    return s;
}

bool ok(const CmdResult& r) { return r.spawned && r.exit_code == 0; }

std::string git_out(const std::string& repo, std::vector<std::string> args) {
    args.insert(args.begin(), {"git", "-C", repo});
    CmdResult r = run(args);
    return ok(r) ? trim(r.out) : "";
}

// True when `git diff --quiet <revs>` finds differences (exit 1).
bool has_diff(const std::string& repo, const std::vector<std::string>& revs) {
    std::vector<std::string> args = {"git", "-C", repo, "diff", "--quiet"};
    args.insert(args.end(), revs.begin(), revs.end());
    CmdResult r = run(args);
    return r.spawned && r.exit_code == 1;
}

int count_lines(const std::string& s) {
    int n = 0;
    for (char c : s)
        if (c == '\n') ++n;
    return n;
}

std::vector<char*> c_argv(std::vector<std::string>& args) {
    std::vector<char*> cargv;
    for (auto& a : args) cargv.push_back(a.data());
    cargv.push_back(nullptr);
    return cargv;
}

// Replace this process with `args`, run from `cwd` if given. Returns only on
// failure.
int exec_in(std::vector<std::string> args, const std::string& cwd = "") {
    std::fflush(stdout);
    std::fflush(stderr);
    if (!cwd.empty() && ::chdir(cwd.c_str()) != 0) {
        std::fprintf(stderr, "lockstep: cannot enter %s: %s\n", cwd.c_str(), std::strerror(errno));
        return 1;
    }
    auto cargv = c_argv(args);
    ::execvp(cargv[0], cargv.data());
    std::fprintf(stderr, "lockstep: could not run %s: %s\n", args[0].c_str(), std::strerror(errno));
    return 1;
}

// Run `args` with our stdout/stderr and wait; its exit code, or -1.
int run_inherit(std::vector<std::string> args) {
    std::fflush(stdout);
    std::fflush(stderr);
    pid_t pid = ::fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        auto cargv = c_argv(args);
        ::execvp(cargv[0], cargv.data());
        ::_exit(127);
    }
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

#ifndef __APPLE__
bool on_path(const std::string& exe) {
    const char* path = std::getenv("PATH");
    if (!path) return false;
    std::string dirs = path;
    for (size_t start = 0; start <= dirs.size();) {
        size_t end = dirs.find(':', start);
        if (end == std::string::npos) end = dirs.size();
        std::string dir = dirs.substr(start, end - start);
        if (!dir.empty() && ::access((dir + "/" + exe).c_str(), X_OK) == 0) return true;
        start = end + 1;
    }
    return false;
}
#endif

#ifdef __APPLE__
bool have_github_desktop() {
    const char* home = std::getenv("HOME");
    for (std::string dir : {std::string("/Applications"),
                            std::string(home ? home : "") + "/Applications"})
        if (std::filesystem::exists(dir + "/GitHub Desktop.app")) return true;
    return false;
}
#endif

// `git gui` is a separate install on some systems (Homebrew's git-gui; Apple's
// git lacks it), so look for it in the git that's on PATH.
bool have_git_gui() {
    std::string exec_path = git_out(".", {"--exec-path"});
    return !exec_path.empty() && ::access((exec_path + "/git-gui").c_str(), X_OK) == 0;
}

}  // namespace

std::optional<std::string> resolve_repo(const std::string& target) {
    std::string top = git_out(target.empty() ? "." : target, {"rev-parse", "--show-toplevel"});
    if (!top.empty()) return top;
    if (target.empty()) {
        std::fprintf(stderr, "lockstep: not in a git repo (pass a repo name or path)\n");
        return std::nullopt;
    }
    for (const auto& p : load_config().repos)
        if (repo_name(p) == target) return p;
    std::fprintf(stderr, "lockstep: '%s' is neither a git repo nor a watched repo name\n",
                 target.c_str());
    return std::nullopt;
}

int cmd_diff(const std::string& target, bool last) {
    auto repo = resolve_repo(target);
    if (!repo) return 1;
    std::string name = repo_name(*repo);

    // Without a tool, difftool falls back to a terminal vimdiff, which is no
    // use when launched from the tray. git itself falls back to merge.tool.
    if (git_out(*repo, {"config", "diff.tool"}).empty() &&
        git_out(*repo, {"config", "merge.tool"}).empty()) {
        std::fprintf(stderr,
                     "lockstep: no diff tool configured; set one with e.g.\n"
                     "  git config --global diff.tool bc\n");
        return 1;
    }

    if (git_out(*repo, {"rev-parse", "--verify", "-q", "HEAD"}).empty()) {
        std::fprintf(stderr, "lockstep: %s has no commits yet\n", name.c_str());
        return 1;
    }

    std::vector<std::string> revs;
    if (last) {
        // First parent (so a merge shows what it brought in); a root commit
        // diffs against the empty tree.
        std::string base = git_out(*repo, {"rev-parse", "--verify", "-q", "HEAD~1"});
        if (base.empty()) base = git_out(*repo, {"hash-object", "-t", "tree", "/dev/null"});
        revs = {base, "HEAD"};
        if (!has_diff(*repo, revs)) {
            std::printf("%s: last commit has no changes\n", name.c_str());
            return 0;
        }
    } else {
        // `git diff HEAD` ignores untracked files, though they count as dirty.
        CmdResult u = run({"git", "-C", *repo, "ls-files", "--others", "--exclude-standard"});
        int untracked = ok(u) ? count_lines(u.out) : 0;
        revs = {"HEAD"};
        if (!has_diff(*repo, revs)) {
            if (untracked)
                std::printf("%s: no changes to tracked files (%d untracked)\n", name.c_str(),
                            untracked);
            else
                std::printf("%s: no changes\n", name.c_str());
            return 0;
        }
        if (untracked)
            std::fprintf(stderr, "note: %d untracked file(s) not shown\n", untracked);
    }

    std::vector<std::string> args = {"git", "-C", *repo, "difftool", "--dir-diff", "--no-prompt"};
    args.insert(args.end(), revs.begin(), revs.end());
    return exec_in(args);
}

int cmd_commit(const std::string& target) {
    auto repo = resolve_repo(target);
    if (!repo) return 1;

#ifdef __APPLE__
    if (have_github_desktop()) return exec_in({"open", "-a", "GitHub Desktop", *repo});
#endif
    if (have_git_gui()) return exec_in({"git", "gui"}, *repo);

    // No commit UI: a terminal in the repo.
#ifdef __APPLE__
    return exec_in({"open", "-a", "Terminal", *repo});
#else
    if (on_path("gnome-terminal"))
        return exec_in({"gnome-terminal", "--working-directory=" + *repo});
    if (on_path("konsole")) return exec_in({"konsole", "--workdir", *repo});
    for (const char* term : {"x-terminal-emulator", "xfce4-terminal", "xterm"})
        if (on_path(term)) return exec_in({term}, *repo);  // opens in its cwd
    std::fprintf(stderr,
                 "lockstep: no commit tool found (looked for git gui and a terminal);\n"
                 "  install git-gui, or commit from a terminal in %s\n",
                 repo->c_str());
    return 1;
#endif
}

int cmd_pull(const std::string& target) {
    auto repo = resolve_repo(target);
    if (!repo) return 1;
    std::string name = repo_name(*repo);

    if (git_out(*repo, {"rev-parse", "--abbrev-ref", "@{upstream}"}).empty()) {
        std::fprintf(stderr, "lockstep: %s's branch has no upstream to pull from\n", name.c_str());
        return 1;
    }

    // From the tray there's no terminal to answer a credential prompt; fail
    // instead of hanging.
    ::setenv("GIT_TERMINAL_PROMPT", "0", 1);
    std::string before = git_out(*repo, {"rev-parse", "HEAD"});
    int code = run_inherit({"git", "-C", *repo, "-c", "advice.diverging=false", "pull", "--quiet",
                            "--no-rebase", "--ff-only"});
    if (code != 0) {
        // The fetch went through, so ahead/behind is current.
        std::string ab = git_out(*repo, {"rev-list", "--count", "--left-right", "@{upstream}...HEAD"});
        bool diverged = ab.find('\t') != std::string::npos && ab.substr(0, ab.find('\t')) != "0" &&
                        ab.substr(ab.find('\t') + 1) != "0";
        std::fprintf(stderr, "lockstep: %s not pulled; nothing was changed.%s\n", name.c_str(),
                     diverged ? " Its branch has diverged from upstream; merge or rebase in a terminal."
                              : "");
        return 1;
    }
    std::string after = git_out(*repo, {"rev-parse", "HEAD"});
    if (before == after) {
        std::printf("%s: already up to date\n", name.c_str());
    } else {
        std::string n = before.empty() ? "" : git_out(*repo, {"rev-list", "--count", before + ".." + after});
        std::printf("%s: pulled %s commit%s\n", name.c_str(), n.empty() ? "new" : n.c_str(),
                    n == "1" ? "" : "s");
    }
    return 0;
}

namespace {

// How long a test run may take before it's killed: $LOCKSTEP_TEST_TIMEOUT
// seconds, default 30 minutes.
int test_timeout_seconds() {
    if (const char* e = std::getenv("LOCKSTEP_TEST_TIMEOUT"); e && std::atoi(e) > 0)
        return std::atoi(e);
    return 30 * 60;
}

struct ShellRun {
    bool spawned = false;
    int exit_code = -1;
    bool timed_out = false;
};

// Run `command` through /bin/sh in `cwd` with stdout and stderr merged, copying
// the output to `log` and, when `echo` is set, to our stdout. Past `timeout`
// seconds the whole process group is killed.
ShellRun run_shell_logged(const std::string& command, const std::string& cwd,
                          std::ofstream& log, bool echo, int timeout) {
    ShellRun result;
    int pipe_fd[2];
    if (::pipe(pipe_fd) < 0) return result;
    std::fflush(stdout);
    pid_t pid = ::fork();
    if (pid < 0) {
        ::close(pipe_fd[0]);
        ::close(pipe_fd[1]);
        return result;
    }
    if (pid == 0) {
        ::setpgid(0, 0);
        int devnull = ::open("/dev/null", O_RDONLY);
        if (devnull >= 0) ::dup2(devnull, STDIN_FILENO);
        ::dup2(pipe_fd[1], STDOUT_FILENO);
        ::dup2(pipe_fd[1], STDERR_FILENO);
        ::close(pipe_fd[0]);
        ::close(pipe_fd[1]);
        if (::chdir(cwd.c_str()) != 0) ::_exit(126);
        ::execl("/bin/sh", "sh", "-c", command.c_str(), static_cast<char*>(nullptr));
        ::_exit(127);
    }
    ::close(pipe_fd[1]);
    result.spawned = true;

    time_t deadline = std::time(nullptr) + timeout;
    char buf[4096];
    for (;;) {
        time_t now = std::time(nullptr);
        if (now >= deadline) {
            result.timed_out = true;
            ::kill(-pid, SIGKILL);
            break;
        }
        struct pollfd pfd{pipe_fd[0], POLLIN, 0};
        int n = ::poll(&pfd, 1, static_cast<int>((deadline - now) * 1000));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) continue;  // timeout: caught at the top of the loop
        ssize_t got = ::read(pipe_fd[0], buf, sizeof(buf));
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) break;  // EOF: every writer has exited
        log.write(buf, got);
        if (echo) std::fwrite(buf, 1, static_cast<size_t>(got), stdout);
    }
    ::close(pipe_fd[0]);
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (WIFEXITED(status)) result.exit_code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) result.exit_code = 128 + WTERMSIG(status);
    return result;
}

// The last `n` non-empty lines of a file, for a failure summary.
std::string tail_lines(const std::string& path, size_t n) {
    std::ifstream in(path);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) {
        if (trim(line).empty()) continue;
        lines.push_back(line);
        if (lines.size() > n) lines.erase(lines.begin());
    }
    std::string out;
    for (const auto& l : lines) out += "  " + l + "\n";
    return out;
}

}  // namespace

int cmd_test(const std::string& target) {
    auto repo = resolve_repo(target);
    if (!repo) return 1;
    std::string name = repo_name(*repo);

    auto ms = load_milestones(*repo);
    std::string command = ms ? ms->test_command() : "";
    if (command.empty()) {
        std::fprintf(stderr,
                     "lockstep: %s has no test command for this platform; add one to %s:\n"
                     "  ## Tests\n  - mac: <command>\n  - linux: <command>\n",
                     name.c_str(), kMilestonesFile);
        return 1;
    }

    std::string log_path = state_dir() + "/tests/" + name + ".log";
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(log_path).parent_path(), ec);
    std::ofstream log(log_path, std::ios::trunc);
    if (!log) {
        std::fprintf(stderr, "lockstep: cannot write %s\n", log_path.c_str());
        return 1;
    }

    TestResult r;
    r.commit = git_out(*repo, {"rev-parse", "--short", "--verify", "-q", "HEAD"});
    r.dirty = !git_out(*repo, {"status", "--porcelain"}).empty();
    r.log = log_path;
    log << "$ " << command << "\n# in " << *repo << " at " << (r.commit.empty() ? "?" : r.commit)
        << (r.dirty ? " (uncommitted changes)" : "") << "\n\n";
    log.flush();

    bool tty = ::isatty(STDOUT_FILENO);
    if (tty) std::printf("%s: %s\n", name.c_str(), command.c_str());
    time_t start = std::time(nullptr);
    ShellRun run = run_shell_logged(command, *repo, log, tty, test_timeout_seconds());
    log.close();
    if (!run.spawned) {
        std::fprintf(stderr, "lockstep: could not start /bin/sh\n");
        return 1;
    }

    r.at = static_cast<int64_t>(std::time(nullptr));
    r.seconds = static_cast<int>(r.at - start);
    r.exit_code = run.exit_code;
    r.timed_out = run.timed_out;
    r.ok = !run.timed_out && run.exit_code == 0;
    if (!save_test_result(name, r))
        std::fprintf(stderr, "lockstep: could not record the result under %s\n",
                     state_dir().c_str());

    std::string when = std::to_string(r.seconds) + "s" +
                       (r.commit.empty() ? "" : ", " + r.commit) + (r.dirty ? "+dirty" : "");
    if (r.ok) {
        std::printf("%s: tests passed (%s)\n", name.c_str(), when.c_str());
        return 0;
    }
    std::string why = run.timed_out ? "timed out" : "exit " + std::to_string(run.exit_code);
    std::fprintf(stderr, "%s: tests FAILED (%s, %s)\n", name.c_str(), why.c_str(), when.c_str());
    if (!tty) std::fprintf(stderr, "%s", tail_lines(log_path, 6).c_str());
    std::fprintf(stderr, "log: %s\n", log_path.c_str());
    return 1;
}

}  // namespace lockstep::cli
