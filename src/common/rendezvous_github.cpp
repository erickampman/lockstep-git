#include "rendezvous.h"

#include <ctime>
#include <fstream>
#include <filesystem>
#include <utility>

#include "subprocess.h"

namespace lockstep {

namespace fs = std::filesystem;

namespace {

constexpr const char* kExt = ".blob";
constexpr int kPushRetries = 3;
// Upper bound on any one git invocation. The daemon is single-threaded, so a
// network stall (e.g. a push hung mid-HTTP) would otherwise wedge it for good.
constexpr int kGitTimeoutSeconds = 60;

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
    return run(argv, kGitTimeoutSeconds);
}

std::string current_branch(const std::string& dir) {
    CmdResult r = git(dir, {"symbolic-ref", "--short", "HEAD"});
    std::string b = trim(r.out);
    return b.empty() ? "main" : b;
}

// A local git failure that isn't a timeout. In this clone that means damage
// (corrupt objects, a stale index.lock left by a crash) — not the network, since
// these commands never touch the remote.
bool local_failure(const CmdResult& r) {
    return r.spawned && !r.timed_out && r.exit_code != 0;
}

// True when the branch exists but HEAD's commit can't be read: the object store
// is damaged. An unborn branch (fresh, empty repo) is fine.
bool head_unreadable(const std::string& dir, const std::string& branch) {
    CmdResult ref = git(dir, {"show-ref", "--verify", "--quiet", "refs/heads/" + branch});
    if (!ref.spawned || ref.exit_code != 0) return false;
    return local_failure(git(dir, {"rev-parse", "--verify", "--quiet", "HEAD^{commit}"}));
}

// Bring the working tree in line with origin's branch tip, discarding any local
// divergence (we regenerate our own blob each publish, and only ever own our own
// file, so there is nothing of value to preserve). No reset when the remote
// branch doesn't exist yet (fresh, empty repo). A failed fetch is tolerated (we
// read the last-known blobs); returns false only when the clone is damaged.
bool sync_to_origin(const std::string& dir, const std::string& branch) {
    git(dir, {"fetch", "--quiet", "origin"});
    if (head_unreadable(dir, branch)) return false;
    CmdResult has = git(dir, {"rev-parse", "--verify", "--quiet",
                              "refs/remotes/origin/" + branch});
    if (has.spawned && has.exit_code == 0 &&
        local_failure(git(dir, {"reset", "--hard", "--quiet", "origin/" + branch})))
        return false;
    return !local_failure(git(dir, {"status", "--porcelain"}));
}

std::string timestamp() {
    std::time_t t = std::time(nullptr);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y%m%d-%H%M%S", std::localtime(&t));
    return buf;
}

// Last re-clone attempt (process-wide: the daemon builds a fresh Rendezvous each
// tick). Limits re-cloning to once per kRecloneIntervalSeconds, so a remote
// that's itself broken can't pile up a `.corrupt-*` directory every tick.
std::time_t g_last_reclone = 0;
constexpr int kRecloneIntervalSeconds = 3600;

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

std::string GitHubRendezvous::take_notice() {
    return std::exchange(notice_, {});
}

bool GitHubRendezvous::reclone(std::string* err) {
    CmdResult url = git(dir_, {"config", "--get", "remote.origin.url"});
    std::string origin = trim(url.out);
    if (!url.spawned || url.exit_code != 0 || origin.empty()) {
        if (err) *err = "rendezvous clone is damaged and has no origin to re-clone from: " + dir_;
        return false;
    }
    std::time_t now = std::time(nullptr);
    if (now - g_last_reclone < kRecloneIntervalSeconds) {
        if (err) *err = "rendezvous clone is damaged (re-clone already tried recently): " + dir_;
        return false;
    }
    g_last_reclone = now;

    // Clone beside the damaged one first, so a failed clone (offline) leaves
    // things as they were.
    std::string fresh = dir_ + ".reclone";
    std::error_code ec;
    fs::remove_all(fresh, ec);
    CmdResult clone = run({"git", "clone", "--quiet", origin, fresh}, kGitTimeoutSeconds);
    if (!clone.spawned || clone.exit_code != 0) {
        fs::remove_all(fresh, ec);
        if (err) *err = "rendezvous clone is damaged; re-clone from " + origin + " failed: " + dir_;
        return false;
    }
    std::string aside = dir_ + ".corrupt-" + timestamp();
    fs::rename(dir_, aside, ec);
    if (ec) {
        fs::remove_all(fresh, ec);
        if (err) *err = "rendezvous clone is damaged; cannot move it aside: " + ec.message();
        return false;
    }
    fs::rename(fresh, dir_, ec);
    if (ec) {
        if (err) *err = "re-cloned rendezvous but cannot move it into place: " + ec.message();
        return false;
    }
    notice_ = "rendezvous clone " + dir_ + " was damaged; re-cloned from origin (old copy at " +
              aside + ")";
    return true;
}

bool GitHubRendezvous::sync(const std::string& branch, std::string* err) {
    if (sync_to_origin(dir_, branch)) return true;
    if (!reclone(err)) return false;
    if (sync_to_origin(dir_, current_branch(dir_))) return true;
    if (err) *err = "rendezvous clone still unreadable after re-clone: " + dir_;
    return false;
}

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

    std::string filename = machine + kExt;

    for (int attempt = 0; attempt < kPushRetries; ++attempt) {
        if (!sync(current_branch(dir_), err)) return false;

        if (!write_blob_file(fs::path(dir_) / filename, blob)) {
            if (err) *err = "cannot write blob in " + dir_;
            return false;
        }
        git(dir_, {"add", "--", filename});

        // If nothing changed vs origin, there's nothing to push — we're current.
        // (A failing status is not "nothing changed": check it really ran.)
        CmdResult st = git(dir_, {"status", "--porcelain"});
        if (st.spawned && st.exit_code == 0 && trim(st.out).empty()) return true;

        if (local_failure(git(dir_, {"commit", "--quiet", "-m", "lockstep: " + machine}))) {
            if (err) *err = "cannot commit blob in rendezvous clone " + dir_;
            return false;
        }

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

    if (!sync(current_branch(dir_), err)) return std::nullopt;

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
