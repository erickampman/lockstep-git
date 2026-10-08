#pragma once

#include <string>

namespace lockstep {

// This machine's state for one watched repo, computed by shelling out to git.
struct RepoState {
    std::string path;            // the configured/expanded repo path
    bool is_repo = false;        // false if path isn't a git work tree
    std::string branch;          // current branch, or "HEAD" if detached
    int dirty = 0;               // count of changed + untracked entries
    bool has_upstream = false;   // true if the branch tracks a remote
    int ahead = 0;               // commits HEAD is ahead of upstream
    int behind = 0;              // commits HEAD is behind upstream
    std::string error;           // non-empty describes why inspection failed
};

// True iff this repo has nothing lockstep would warn about locally:
// no dirty files, and (if tracking) not ahead of its upstream.
bool is_clean(const RepoState& s);

// Inspect `repo_path` by running git in it. Never throws; failures are
// reported via RepoState::error / is_repo=false.
RepoState inspect(const std::string& repo_path);

}  // namespace lockstep
