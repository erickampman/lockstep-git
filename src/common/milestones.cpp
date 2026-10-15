#include "milestones.h"

#include <unistd.h>

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>

namespace lockstep {

namespace {

std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r");
    return s.substr(b, e - b + 1);
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// A command written as `cmd` in the file is just cmd.
std::string unquote(std::string s) {
    s = trim(s);
    if (s.size() >= 2 && s.front() == '`' && s.back() == '`') s = s.substr(1, s.size() - 2);
    return trim(s);
}

std::string read_file(const std::filesystem::path& p, bool* found = nullptr) {
    std::ifstream in(p);
    if (found) *found = bool(in);
    if (!in) return "";
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::filesystem::path tests_dir() { return std::filesystem::path(state_dir()) / "tests"; }

}  // namespace

int Milestone::done_count() const {
    int n = 0;
    for (const auto& i : items)
        if (i.done) ++n;
    return n;
}

std::string Milestones::test_command() const {
#ifdef __APPLE__
    const std::string& mine = test_mac;
#else
    const std::string& mine = test_linux;
#endif
    return mine.empty() ? test_all : mine;
}

int Milestones::current() const {
    for (size_t i = 0; i < milestones.size(); ++i)
        if (!milestones[i].complete()) return int(i);
    return -1;
}

Milestones parse_milestones(const std::string& text) {
    static const std::regex heading(R"(^##\s+(.*)$)");
    static const std::regex due(R"(\(\s*due:?\s*(\d{4}-\d{2}-\d{2})\s*\))", std::regex::icase);
    static const std::regex item(R"(^\s*[-*+]\s+\[([ xX])\]\s+(.*)$)");
    static const std::regex test(R"(^\s*[-*+]\s+([A-Za-z]+)\s*:\s*(.+)$)");

    Milestones out;
    enum { None, Tests, InMilestone } section = None;
    bool in_fence = false;
    std::istringstream in(text);
    std::string line;
    std::smatch m;
    while (std::getline(in, line)) {
        if (trim(line).rfind("```", 0) == 0) {
            in_fence = !in_fence;
            continue;
        }
        if (in_fence) continue;

        if (std::regex_match(line, m, heading)) {
            std::string title = trim(m[1]);
            if (lower(title) == "tests") {
                section = Tests;
                continue;
            }
            Milestone ms;
            std::smatch d;
            if (std::regex_search(title, d, due)) {
                ms.due = d[1];
                title = trim(d.prefix().str() + d.suffix().str());
            }
            ms.title = title;
            out.milestones.push_back(std::move(ms));
            section = InMilestone;
            continue;
        }
        if (line.rfind("# ", 0) == 0) {  // a top-level title ends any section
            section = None;
            continue;
        }

        if (section == InMilestone && std::regex_match(line, m, item)) {
            out.milestones.back().items.push_back({trim(m[2]), m[1] != " "});
        } else if (section == Tests && std::regex_match(line, m, test)) {
            std::string platform = lower(m[1]);
            std::string cmd = unquote(m[2]);
            if (platform == "mac" || platform == "macos") out.test_mac = cmd;
            else if (platform == "linux") out.test_linux = cmd;
            else if (platform == "all") out.test_all = cmd;
        }
    }
    return out;
}

std::optional<Milestones> load_milestones(const std::string& repo_path) {
    bool found = false;
    std::string text = read_file(std::filesystem::path(repo_path) / kMilestonesFile, &found);
    if (!found) return std::nullopt;
    return parse_milestones(text);
}

nlohmann::json milestone_summary(const std::string& repo_path) {
    auto ms = load_milestones(repo_path);
    if (!ms) return nullptr;
    int count = int(ms->milestones.size());
    int cur = ms->current();
    bool has_tests = !ms->test_command().empty();
    if (cur < 0) return {{"all_done", count > 0}, {"count", count}, {"has_tests", has_tests}};
    const Milestone& m = ms->milestones[cur];
    return {{"title", m.title},
            {"due", m.due},
            {"done", m.done_count()},
            {"total", int(m.items.size())},
            {"index", cur},
            {"count", count},
            {"has_tests", has_tests}};
}

std::string state_dir() {
    std::filesystem::path dir;
    if (const char* x = std::getenv("XDG_STATE_HOME"); x && *x) {
        dir = std::filesystem::path(x) / "lockstep";
    } else {
        const char* home = std::getenv("HOME");
        dir = std::filesystem::path(home && *home ? home : ".") / ".local" / "state" / "lockstep";
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir.string();
}

std::optional<TestResult> load_test_result(const std::string& repo_name) {
    bool found = false;
    std::string text = read_file(tests_dir() / (repo_name + ".json"), &found);
    if (!found) return std::nullopt;
    auto j = nlohmann::json::parse(text, nullptr, false);
    if (!j.is_object()) return std::nullopt;
    return test_result_from_json(j);
}

bool save_test_result(const std::string& repo_name, const TestResult& r) {
    std::error_code ec;
    std::filesystem::create_directories(tests_dir(), ec);
    // Write then rename, so the daemon never reads half a file.
    auto final_path = tests_dir() / (repo_name + ".json");
    auto tmp = tests_dir() / (repo_name + ".json.tmp" + std::to_string(::getpid()));
    {
        std::ofstream out(tmp);
        if (!out) return false;
        out << to_json(r).dump(2) << "\n";
        if (!out) return false;
    }
    std::filesystem::rename(tmp, final_path, ec);
    return !ec;
}

nlohmann::json to_json(const TestResult& r) {
    return {{"ok", r.ok},         {"exit", r.exit_code}, {"timed_out", r.timed_out},
            {"at", r.at},         {"seconds", r.seconds}, {"commit", r.commit},
            {"dirty", r.dirty},   {"log", r.log}};
}

TestResult test_result_from_json(const nlohmann::json& j) {
    TestResult r;
    r.ok = j.value("ok", false);
    r.exit_code = j.value("exit", -1);
    r.timed_out = j.value("timed_out", false);
    r.at = j.value("at", int64_t{0});
    r.seconds = j.value("seconds", 0);
    r.commit = j.value("commit", "");
    r.dirty = j.value("dirty", false);
    r.log = j.value("log", "");
    return r;
}

}  // namespace lockstep
