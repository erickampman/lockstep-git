#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace lockstep {

// Where encrypted machine blobs are published and read. Each machine owns one
// slot keyed by its machine id; values are opaque encrypted bytes (the backend
// never sees plaintext). The GitHub-private-repo backend (one file per machine,
// committed + pushed) and the local FileRendezvous below are interchangeable.
class Rendezvous {
public:
    virtual ~Rendezvous() = default;

    // Store `blob` as this machine's published slot. Overwrites the prior one.
    virtual bool publish(const std::string& machine,
                         const std::vector<uint8_t>& blob,
                         std::string* err) = 0;

    // Fetch every OTHER machine's slot (excluding `self`), keyed by machine id.
    // An empty map means "no other machine has published yet" (not an error).
    virtual std::optional<std::map<std::string, std::vector<uint8_t>>>
    fetch_others(const std::string& self, std::string* err) = 0;
};

// Local-filesystem backend: one `<machine>.blob` file per machine under `dir`.
// Used for tests and as the reference implementation; the GitHub backend layers
// commit/push/pull over the same shape.
class FileRendezvous : public Rendezvous {
public:
    explicit FileRendezvous(std::string dir);

    bool publish(const std::string& machine, const std::vector<uint8_t>& blob,
                 std::string* err) override;
    std::optional<std::map<std::string, std::vector<uint8_t>>>
    fetch_others(const std::string& self, std::string* err) override;

private:
    std::string dir_;
};

// GitHub (or any git remote) backend: a local clone of a *private* repo whose
// working tree is exactly the FileRendezvous shape (one `<machine>.blob` per
// machine). publish syncs to origin, writes this machine's blob, commits and
// pushes (retrying on a concurrent push); fetch_others pulls, then reads the
// other machines' blobs. Each machine only ever writes its own file, so the
// only conflict is a non-fast-forward push, which the retry resolves.
//
// The daemon sets GIT_TERMINAL_PROMPT=0 so a missing credential fails fast
// instead of hanging the daemon on an interactive prompt.
class GitHubRendezvous : public Rendezvous {
public:
    explicit GitHubRendezvous(std::string clone_dir);

    bool publish(const std::string& machine, const std::vector<uint8_t>& blob,
                 std::string* err) override;
    std::optional<std::map<std::string, std::vector<uint8_t>>>
    fetch_others(const std::string& self, std::string* err) override;

private:
    std::string dir_;
};

}  // namespace lockstep
