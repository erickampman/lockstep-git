#include "diff.h"

#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
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
    std::vector<char*> cargv;
    for (auto& a : args) cargv.push_back(a.data());
    cargv.push_back(nullptr);

    std::fflush(stdout);
    std::fflush(stderr);
    ::execvp(cargv[0], cargv.data());
    std::fprintf(stderr, "lockstep: could not run git: %s\n", std::strerror(errno));
    return 1;
}

}  // namespace lockstep::cli
