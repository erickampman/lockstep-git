#pragma once

// `lockstep diff [<name|path>] [--last]`: open this machine's changes in the
// visual tool git is configured with (diff.tool), via `git difftool --dir-diff`.
// Without --last: the working tree (staged + unstaged) against HEAD. With
// --last: the last commit against its first parent. Local only; nothing goes
// through the daemon or the rendezvous.
#include <string>

namespace lockstep::cli {

// `target` is a repo path, a watched repo's name, or empty for the repo
// containing the current directory. On success this replaces the process with
// git difftool and does not return.
int cmd_diff(const std::string& target, bool last);

}  // namespace lockstep::cli
