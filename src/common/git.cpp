#include "git.h"

#include <cstdlib>
#include <string>

#include "subprocess.h"

namespace lockstep {

namespace {

// Trim trailing newlines/spaces from git's single-line outputs.
std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
        s.pop_back();
    return s;
}

int count_lines(const std::string& s) {
    int n = 0;
    for (char c : s)
        if (c == '\n') ++n;
    return n;
}

}  // namespace

bool is_clean(const RepoState& s) {
    if (!s.error.empty() || !s.is_repo) return false;
    return s.dirty == 0 && !(s.has_upstream && s.ahead > 0);
}

RepoState inspect(const std::string& repo_path) {
    RepoState st;
    st.path = repo_path;

    // Is this a git work tree? (Also fails cleanly if the path doesn't exist.)
    CmdResult check =
        run({"git", "-C", repo_path, "rev-parse", "--is-inside-work-tree"});
    if (!check.spawned) {
        st.error = "could not run git";
        return st;
    }
    if (check.exit_code != 0 || trim(check.out) != "true") {
        st.is_repo = false;
        st.error = "not a git work tree";
        return st;
    }
    st.is_repo = true;

    // Current branch name (or "HEAD" when detached).
    CmdResult br = run({"git", "-C", repo_path, "rev-parse", "--abbrev-ref", "HEAD"});
    if (br.spawned && br.exit_code == 0) st.branch = trim(br.out);

    // Dirty entries: one porcelain line per changed/untracked path.
    CmdResult status = run({"git", "-C", repo_path, "status", "--porcelain"});
    if (status.spawned && status.exit_code == 0) {
        std::string out = status.out;
        if (!out.empty() && out.back() != '\n') out.push_back('\n');
        st.dirty = count_lines(out);
    }

    // Ahead/behind vs upstream. `--left-right @{u}...HEAD` prints "behind<TAB>ahead";
    // a non-zero exit means the branch has no upstream.
    CmdResult ab = run({"git", "-C", repo_path, "rev-list", "--count",
                        "--left-right", "@{upstream}...HEAD"});
    if (ab.spawned && ab.exit_code == 0) {
        st.has_upstream = true;
        std::string s = trim(ab.out);
        // Parse "<behind>\t<ahead>".
        auto tab = s.find('\t');
        if (tab != std::string::npos) {
            st.behind = std::atoi(s.substr(0, tab).c_str());
            st.ahead = std::atoi(s.substr(tab + 1).c_str());
        }
    }

    return st;
}

}  // namespace lockstep
