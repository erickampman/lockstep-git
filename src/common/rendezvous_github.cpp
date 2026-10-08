#include "rendezvous.h"

#include <fstream>
#include <filesystem>

#include "subprocess.h"

namespace lockstep {

namespace fs = std::filesystem;

namespace {

constexpr const char* kExt = ".blob";
constexpr int kPushRetries = 3;

bool safe_machine(const std::string& m) {
    if (m.empty() || m == "." || m == "..") return false;
    return m.find('/') == std::string::npos && m.find('\\') == std::string::npos;
}

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
        s.pop_back();
    return s;
}

// Run `git -C dir <args...>` with stdout captured.
CmdResult git(const std::string& dir, std::vector<std::string> args) {
    std::vector<std::string> argv = {"git", "-C", dir};
    argv.insert(argv.end(), args.begin(), args.end());
    return run(argv);
}

std::string current_branch(const std::string& dir) {
    CmdResult r = git(dir, {"symbolic-ref", "--short", "HEAD"});
    std::string b = trim(r.out);
    return b.empty() ? "main" : b;
}

// Bring the working tree in line with origin's branch tip, discarding any local
// divergence (we regenerate our own blob each publish, and only ever own our own
// file, so there is nothing of value to preserve). No-op when the remote branch
// doesn't exist yet (fresh, empty repo).
void sync_to_origin(const std::string& dir, const std::string& branch) {
    git(dir, {"fetch", "--quiet", "origin"});
    CmdResult has = git(dir, {"rev-parse", "--verify", "--quiet",
                              "refs/remotes/origin/" + branch});
    if (has.spawned && has.exit_code == 0) {
        git(dir, {"reset", "--hard", "--quiet", "origin/" + branch});
    }
}

bool write_blob_file(const fs::path& p, const std::vector<uint8_t>& blob) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(blob.data()),
              static_cast<std::streamsize>(blob.size()));
    return static_cast<bool>(out);
}

std::vector<uint8_t> read_blob_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

}  // namespace

GitHubRendezvous::GitHubRendezvous(std::string clone_dir)
    : dir_(std::move(clone_dir)) {}

bool GitHubRendezvous::publish(const std::string& machine,
                               const std::vector<uint8_t>& blob, std::string* err) {
    if (!safe_machine(machine)) {
        if (err) *err = "unsafe machine id: " + machine;
        return false;
    }
    CmdResult chk = git(dir_, {"rev-parse", "--is-inside-work-tree"});
    if (!chk.spawned || chk.exit_code != 0 || trim(chk.out) != "true") {
        if (err) *err = "rendezvous repo is not a git work tree: " + dir_;
        return false;
    }

    std::string branch = current_branch(dir_);
    std::string filename = machine + kExt;

    for (int attempt = 0; attempt < kPushRetries; ++attempt) {
        sync_to_origin(dir_, branch);

        if (!write_blob_file(fs::path(dir_) / filename, blob)) {
            if (err) *err = "cannot write blob in " + dir_;
            return false;
        }
        git(dir_, {"add", "--", filename});

        // If nothing changed vs origin, there's nothing to push — we're current.
        CmdResult st = git(dir_, {"status", "--porcelain"});
        if (st.spawned && trim(st.out).empty()) return true;

        git(dir_, {"commit", "--quiet", "-m", "lockstep: " + machine});

        CmdResult push = git(dir_, {"push", "--quiet", "-u", "origin", "HEAD"});
        if (push.spawned && push.exit_code == 0) return true;
        // Push rejected (a concurrent push landed first): loop to re-sync + retry.
    }

    if (err) *err = "push to rendezvous failed after retries (auth or conflict)";
    return false;
}

std::optional<std::map<std::string, std::vector<uint8_t>>>
GitHubRendezvous::fetch_others(const std::string& self, std::string* err) {
    CmdResult chk = git(dir_, {"rev-parse", "--is-inside-work-tree"});
    if (!chk.spawned || chk.exit_code != 0 || trim(chk.out) != "true") {
        if (err) *err = "rendezvous repo is not a git work tree: " + dir_;
        return std::nullopt;
    }

    sync_to_origin(dir_, current_branch(dir_));

    std::map<std::string, std::vector<uint8_t>> out;
    std::error_code ec;
    if (!fs::exists(dir_, ec)) return out;
    for (const auto& entry : fs::directory_iterator(dir_, ec)) {
        if (ec) {
            if (err) *err = "cannot read rendezvous dir: " + ec.message();
            return std::nullopt;
        }
        const fs::path& p = entry.path();
        if (p.extension() != kExt) continue;
        std::string machine = p.stem().string();
        if (machine == self) continue;
        out[machine] = read_blob_file(p);
    }
    return out;
}

}  // namespace lockstep
