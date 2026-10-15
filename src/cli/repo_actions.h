#pragma once

// Per-repo actions on this machine, used from a terminal and by the tray. Each
// takes `target`: a repo path, a watched repo's name, or empty for the repo
// containing the current directory. All are local only; nothing goes through
// the daemon or the rendezvous.
#include <optional>
#include <string>

namespace lockstep::cli {

// Resolve `target` to a repo's top-level directory: empty means the current
// directory's repo; otherwise a path (any repo, watched or not), then a watched
// repo's name. Explains on stderr and returns nullopt when it's neither.
std::optional<std::string> resolve_repo(const std::string& target);

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

// `lockstep test [<name|path>]`: run the repo's test command for this platform,
// from the "## Tests" section of its MILESTONES.md, through /bin/sh in the repo
// root. Output goes to a log under the state dir (and the terminal, if there is
// one); pass/fail is recorded for the tray and the other machines to see.
int cmd_test(const std::string& target);

}  // namespace lockstep::cli
