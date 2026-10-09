#pragma once

// Per-repo actions on this machine, used from a terminal and by the tray. Each
// takes `target`: a repo path, a watched repo's name, or empty for the repo
// containing the current directory. All are local only; nothing goes through
// the daemon or the rendezvous.
#include <string>

namespace lockstep::cli {

// `lockstep diff [<name|path>] [--last]`: open the changes in the visual tool
// git is configured with (diff.tool), via `git difftool --dir-diff`. Without
// --last: the working tree (staged + unstaged) against HEAD. With --last: the
// last commit against its first parent. On success this replaces the process
// with git difftool and does not return.
int cmd_diff(const std::string& target, bool last);

// `lockstep commit [<name|path>]`: open a UI to review and commit — never a
// blind commit. The first one found: GitHub Desktop (macOS), `git gui`, else a
// terminal window in the repo. Hooks still run, so the guard applies.
int cmd_commit(const std::string& target);

// `lockstep pull [<name|path>]`: fast-forward only, whatever pull.rebase says,
// so a pull started from the tray never leaves a merge or rebase half done.
// Diverged branches or conflicting local edits are refused untouched.
int cmd_pull(const std::string& target);

}  // namespace lockstep::cli
