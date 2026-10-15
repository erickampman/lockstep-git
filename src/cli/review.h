#pragma once

#include <string>

namespace lockstep::cli {

// `lockstep review [<name|path>]`: where each repo stands against its
// MILESTONES.md: the current milestone and its open items, the commits since
// the file last changed (work that may have finished items nobody ticked), and
// the last test result on every machine. With no target, every watched repo.
// Markdown on stdout, so it reads in a terminal and feeds an AI reviewer as-is.
int cmd_review(const std::string& target);

}  // namespace lockstep::cli
