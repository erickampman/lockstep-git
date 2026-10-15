#include "review.h"

#include <cmath>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "blob.h"
#include "config.h"
#include "git.h"
#include "ipc.h"
#include "milestones.h"
#include "paths.h"
#include "repo_actions.h"
#include "subprocess.h"

using nlohmann::json;

namespace lockstep::cli {

namespace {

// Commits listed per repo before the rest are summarized as a count.
constexpr int kMaxCommits = 25;

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
        s.pop_back();
    return s;
}

std::string git_out(const std::string& repo, std::vector<std::string> args) {
    args.insert(args.begin(), {"git", "-C", repo});
    CmdResult r = run(args, 30);
    return r.spawned && r.exit_code == 0 ? trim(r.out) : "";
}

std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start < s.size()) {
        size_t nl = s.find('\n', start);
        if (nl == std::string::npos) nl = s.size();
        if (nl > start) out.push_back(s.substr(start, nl - start));
        start = nl + 1;
    }
    return out;
}

// "in 12 days", "today", "3 days overdue" for a YYYY-MM-DD date.
std::string due_in(const std::string& date) {
    std::tm due{};
    if (std::sscanf(date.c_str(), "%d-%d-%d", &due.tm_year, &due.tm_mon, &due.tm_mday) != 3)
        return "";
    due.tm_year -= 1900;
    due.tm_mon -= 1;
    due.tm_hour = 12;
    std::time_t now = std::time(nullptr);
    std::tm today = *std::localtime(&now);
    today.tm_hour = 12;
    today.tm_min = today.tm_sec = 0;
    long days = std::lround(std::difftime(std::mktime(&due), std::mktime(&today)) / 86400.0);
    if (days == 0) return "today";
    if (days > 0) return "in " + std::to_string(days) + (days == 1 ? " day" : " days");
    return std::to_string(-days) + (days == -1 ? " day" : " days") + " overdue";
}

std::string describe_git(const RepoState& s) {
    if (!s.error.empty()) return s.error;
    std::string out = s.branch + " · " + (s.dirty ? std::to_string(s.dirty) + " uncommitted" : "clean");
    if (!s.has_upstream) return out + ", no upstream";
    if (s.ahead) out += ", " + std::to_string(s.ahead) + " unpushed";
    if (s.behind) out += ", " + std::to_string(s.behind) + " behind";
    if (!s.ahead && !s.behind) out += ", up to date";
    return out;
}

std::string describe_test(bool ok, const std::string& as_of, bool current) {
    return std::string(ok ? "passed" : "FAILED") + " " + as_of +
           (current ? "" : " (older code or uncommitted changes)");
}

std::string human_age(int64_t ts) {
    int64_t secs = static_cast<int64_t>(std::time(nullptr)) - ts;
    if (secs < 0) secs = 0;
    if (secs < 90) return std::to_string(secs) + "s ago";
    if (secs < 90 * 60) return std::to_string(secs / 60) + "m ago";
    if (secs < 48 * 3600) return std::to_string(secs / 3600) + "h ago";
    return std::to_string(secs / 86400) + "d ago";
}

void review_one(const std::string& path, const std::string& machine, const json& others) {
    std::string name = repo_name(path);
    RepoState s = inspect(path);
    std::printf("## %s\n\n", name.c_str());
    std::printf("- git: %s\n", describe_git(s).c_str());

    // Tests, on this machine and wherever else this repo is watched.
    std::string tests;
    if (auto t = load_test_result(name))
        tests = machine + " " +
                describe_test(t->ok, human_age(t->at), !t->dirty && t->commit == s.head);
    else
        tests = machine + " not run";
    for (const auto& m : others) {
        for (const auto& r : m.value("repos", json::array())) {
            if (r.value("name", "") != name) continue;
            tests += "; " + m.value("machine", "?") + " ";
            if (auto t = r.find("test"); t != r.end() && t->is_object())
                tests += describe_test(t->value("ok", false), t->value("as_of", "?"),
                                       t->value("current", false));
            else
                tests += "not run";
        }
    }
    std::printf("- tests: %s\n", tests.c_str());

    auto ms = load_milestones(path);
    if (!ms) {
        std::printf("- no %s\n\n", kMilestonesFile);
        return;
    }
    if (ms->test_command().empty()) std::printf("- no test command for this platform\n");

    int cur = ms->current();
    int count = int(ms->milestones.size());
    if (count == 0) {
        std::printf("- %s lists no milestones\n", kMilestonesFile);
    } else if (cur < 0) {
        std::printf("- all %d milestone%s complete\n", count, count == 1 ? "" : "s");
    } else {
        const Milestone& m = ms->milestones[cur];
        std::printf("- milestone %d of %d: **%s**, %d/%d done", cur + 1, count, m.title.c_str(),
                    m.done_count(), int(m.items.size()));
        if (!m.due.empty()) {
            std::string when = due_in(m.due);
            std::printf(", due %s%s", m.due.c_str(), when.empty() ? "" : (" (" + when + ")").c_str());
        }
        std::printf("\n");
        if (m.items.empty()) std::printf("  - (no checklist items)\n");
        for (const auto& i : m.items)
            if (!i.done) std::printf("  - [ ] %s\n", i.text.c_str());
        if (cur + 1 < count)
            std::printf("- next: %s\n", ms->milestones[cur + 1].title.c_str());
    }

    // Work since the plan was last touched: the place to look for items that
    // are done but not yet ticked.
    std::string since = git_out(path, {"log", "-1", "--format=%h %ad", "--date=short", "--",
                                       kMilestonesFile});
    std::vector<std::string> range;
    std::string heading;
    if (since.empty()) {
        range = {"--since=30.days"};
        heading = std::string(kMilestonesFile) + " isn't committed yet; commits from the last 30 days";
    } else {
        range = {since.substr(0, since.find(' ')) + "..HEAD"};
        heading = "commits since " + std::string(kMilestonesFile) + " last changed (" +
                  since.substr(since.find(' ') + 1) + ")";
    }
    std::vector<std::string> args = {"log", "--format=%h %ad %s", "--date=short"};
    args.insert(args.end(), range.begin(), range.end());
    auto commits = split_lines(git_out(path, args));
    if (commits.empty()) {
        std::printf("- %s: none\n", heading.c_str());
    } else {
        std::printf("- %s: %zu\n", heading.c_str(), commits.size());
        for (size_t i = 0; i < commits.size() && i < kMaxCommits; ++i)
            std::printf("  - %s\n", commits[i].c_str());
        if (commits.size() > kMaxCommits)
            std::printf("  - … and %zu older\n", commits.size() - kMaxCommits);
    }
    std::printf("\n");
}

}  // namespace

int cmd_review(const std::string& target) {
    Config cfg = load_config();
    std::vector<std::string> repos;
    if (target.empty()) {
        repos = cfg.repos;
        if (repos.empty()) {
            std::fprintf(stderr, "lockstep: no repos watched\n");
            return 1;
        }
    } else {
        auto repo = resolve_repo(target);
        if (!repo) return 1;
        repos = {*repo};
    }

    // The other machines' test results come from the daemon's last sync.
    json others = json::array();
    std::string sync_note;
    if (auto reply = ipc::request(socket_path(), {{"cmd", ipc::kCmdStatus}})) {
        others = reply->value("others", json::array());
        if (json sync = reply->value("sync", json::object()); sync.contains("last_sync"))
            sync_note = "other machines as of their last sync, " +
                        sync.value("last_sync", std::string()) + ".";
    } else {
        sync_note = "daemon not reachable, so other machines aren't shown.";
    }

    char today[16];
    std::time_t now = std::time(nullptr);
    std::strftime(today, sizeof(today), "%Y-%m-%d", std::localtime(&now));
    std::printf("# Milestone review, %s\n\n", today);
    std::printf("From %s; %s\n\n", machine_id(cfg).c_str(), sync_note.c_str());
    for (const auto& path : repos) review_one(path, machine_id(cfg), others);
    return 0;
}

}  // namespace lockstep::cli
