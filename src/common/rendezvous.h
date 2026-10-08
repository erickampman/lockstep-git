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

}  // namespace lockstep
