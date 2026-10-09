#include "repo_actions.h"

#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "config.h"
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

// Resolve `target` to a repo's top-level directory: empty means the current
// directory's repo; otherwise a path (any repo, watched or not), then a watched
// repo's name.
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

}  // namespace lockstep::cli
