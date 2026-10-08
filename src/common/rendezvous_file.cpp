#include "rendezvous.h"

#include <fstream>
#include <filesystem>
#include <system_error>

namespace lockstep {

namespace fs = std::filesystem;

namespace {

constexpr const char* kExt = ".blob";

// A machine id must map to a safe single filename. Reject anything with a path
// separator or traversal so a crafted blob can't write outside `dir`.
bool safe_machine(const std::string& m) {
    if (m.empty() || m == "." || m == "..") return false;
    return m.find('/') == std::string::npos && m.find('\\') == std::string::npos;
}

std::vector<uint8_t> read_all(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

}  // namespace

FileRendezvous::FileRendezvous(std::string dir) : dir_(std::move(dir)) {}

bool FileRendezvous::publish(const std::string& machine,
                             const std::vector<uint8_t>& blob, std::string* err) {
    if (!safe_machine(machine)) {
        if (err) *err = "unsafe machine id: " + machine;
        return false;
    }
    std::error_code ec;
    fs::create_directories(dir_, ec);

    // Write to a temp file then rename, so a reader never sees a half-written slot.
    fs::path final = fs::path(dir_) / (machine + kExt);
    fs::path tmp = fs::path(dir_) / (machine + kExt + ".tmp");
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            if (err) *err = "cannot write " + tmp.string();
            return false;
        }
        out.write(reinterpret_cast<const char*>(blob.data()),
                  static_cast<std::streamsize>(blob.size()));
        if (!out) {
            if (err) *err = "write failed: " + tmp.string();
            return false;
        }
    }
    fs::rename(tmp, final, ec);
    if (ec) {
        if (err) *err = "rename failed: " + ec.message();
        return false;
    }
    return true;
}

std::optional<std::map<std::string, std::vector<uint8_t>>>
FileRendezvous::fetch_others(const std::string& self, std::string* err) {
    std::map<std::string, std::vector<uint8_t>> out;
    std::error_code ec;
    if (!fs::exists(dir_, ec)) return out;  // nobody has published yet

    for (const auto& entry : fs::directory_iterator(dir_, ec)) {
        if (ec) {
            if (err) *err = "cannot read rendezvous dir: " + ec.message();
            return std::nullopt;
        }
        const fs::path& p = entry.path();
        if (p.extension() != kExt) continue;
        std::string machine = p.stem().string();
        if (machine == self) continue;  // skip our own slot
        out[machine] = read_all(p);
    }
    return out;
}

}  // namespace lockstep
