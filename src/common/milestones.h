#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

// MILESTONES.md: a repo's plan, read by lockstep for the tray, `lockstep test`
// and `lockstep review`. Plain Markdown so it reads fine on GitHub too:
//
//   ## Tests
//   - mac: swift test
//   - linux: cmake --build build && ctest --test-dir build
//
//   ## M3: Preset browser (due 2026-11-15)
//   - [x] Load presets from disk
//   - [ ] Browser UI
//
// Every `##` heading except "Tests" is a milestone, in file order. The current
// milestone is the first one not complete; complete means it has items and all
// are checked. "(due YYYY-MM-DD)" in a heading is its target date. Test lines
// are `- <platform>: <command>`, platform one of mac/macos, linux or all.
namespace lockstep {

struct MilestoneItem {
    std::string text;
    bool done = false;
};

struct Milestone {
    std::string title;  // heading text, minus any "(due ...)"
    std::string due;    // "YYYY-MM-DD", or empty
    std::vector<MilestoneItem> items;
    int done_count() const;
    bool complete() const { return !items.empty() && done_count() == int(items.size()); }
};

struct Milestones {
    std::vector<Milestone> milestones;
    std::string test_mac, test_linux, test_all;
    // The test command for this platform (its own line, else "all"), or empty.
    std::string test_command() const;
    // Index of the current milestone, or -1 when there are none or all are done.
    int current() const;
};

inline constexpr const char* kMilestonesFile = "MILESTONES.md";

// Parse MILESTONES.md text. Never fails; unrecognized lines are ignored.
Milestones parse_milestones(const std::string& text);

// Read <repo>/MILESTONES.md. nullopt when the repo has none.
std::optional<Milestones> load_milestones(const std::string& repo_path);

// Summary of the current milestone for status replies:
// {title, due, done, total, index, count, has_tests} — or {all_done, count,
// has_tests} when no milestone is open. Null when there is no MILESTONES.md.
nlohmann::json milestone_summary(const std::string& repo_path);

// --- Test results ----------------------------------------------------------
// `lockstep test` records each run; the daemon reads the record into status
// replies and the published blob (pass/fail and when, never the output).

struct TestResult {
    bool ok = false;
    int exit_code = -1;
    bool timed_out = false;
    int64_t at = 0;       // unix seconds the run finished
    int seconds = 0;      // how long it took
    std::string commit;   // short HEAD it ran against
    bool dirty = false;   // working tree had uncommitted changes
    std::string log;      // full output, on this machine only
};

// $XDG_STATE_HOME/lockstep, else ~/.local/state/lockstep. Created if missing.
std::string state_dir();

// Last recorded run for a repo (by name), if any.
std::optional<TestResult> load_test_result(const std::string& repo_name);

// Record a run for a repo, replacing its previous one.
bool save_test_result(const std::string& repo_name, const TestResult& r);

nlohmann::json to_json(const TestResult& r);
TestResult test_result_from_json(const nlohmann::json& j);

}  // namespace lockstep
