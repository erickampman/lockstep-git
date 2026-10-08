#include "crypto.h"

#include <sodium.h>
#include <sys/stat.h>

#include <cctype>
#include <cstdio>
#include <fstream>

namespace lockstep::crypto {

namespace {

std::string to_hex(const std::vector<uint8_t>& bytes) {
    static const char* d = "0123456789abcdef";
    std::string s;
    s.reserve(bytes.size() * 2);
    for (uint8_t b : bytes) {
        s.push_back(d[b >> 4]);
        s.push_back(d[b & 0x0f]);
    }
    return s;
}

std::optional<std::vector<uint8_t>> from_hex(const std::string& hex) {
    std::string clean;
    clean.reserve(hex.size());
    for (char c : hex) {
        if (std::isspace(static_cast<unsigned char>(c))) continue;
        clean.push_back(c);
    }
    if (clean.size() % 2 != 0) return std::nullopt;
    std::vector<uint8_t> out;
    out.reserve(clean.size() / 2);
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < clean.size(); i += 2) {
        int hi = nibble(clean[i]);
        int lo = nibble(clean[i + 1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return out;
}

}  // namespace

bool init() { return sodium_init() >= 0; }

Key generate_key() {
    Key key(kKeyBytes);
    randombytes_buf(key.data(), key.size());
    return key;
}

std::optional<Key> load_key(const std::string& path, std::string* err) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (err) *err = "cannot open key file: " + path;
        return std::nullopt;
    }
    std::string contents((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
    auto bytes = from_hex(contents);
    if (!bytes || bytes->size() != kKeyBytes) {
        if (err) *err = "key file is not a valid 32-byte hex key: " + path;
        return std::nullopt;
    }
    return bytes;
}

bool save_key(const std::string& path, const Key& key, std::string* err) {
    // Create with 0600 from the start so the key is never briefly world-readable.
    std::ofstream out;
    {
        FILE* f = std::fopen(path.c_str(), "wb");
        if (!f) {
            if (err) *err = "cannot create key file: " + path;
            return false;
        }
        std::fclose(f);
    }
    ::chmod(path.c_str(), 0600);
    out.open(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        if (err) *err = "cannot write key file: " + path;
        return false;
    }
    out << to_hex(key) << '\n';
    return static_cast<bool>(out);
}

std::optional<std::vector<uint8_t>> encrypt(const Key& key,
                                            const std::string& plaintext) {
    if (key.size() != kKeyBytes) return std::nullopt;

    std::vector<uint8_t> blob(kNonceBytes + plaintext.size() +
                              crypto_aead_xchacha20poly1305_ietf_ABYTES);
    randombytes_buf(blob.data(), kNonceBytes);  // nonce prefix

    unsigned long long clen = 0;
    crypto_aead_xchacha20poly1305_ietf_encrypt(
        blob.data() + kNonceBytes, &clen,
        reinterpret_cast<const unsigned char*>(plaintext.data()), plaintext.size(),
        nullptr, 0,          // no additional data
        nullptr,             // nsec (unused)
        blob.data(),         // nonce
        key.data());
    blob.resize(kNonceBytes + clen);
    return blob;
}

std::optional<std::string> decrypt(const Key& key,
                                   const std::vector<uint8_t>& blob) {
    if (key.size() != kKeyBytes) return std::nullopt;
    if (blob.size() < kNonceBytes + crypto_aead_xchacha20poly1305_ietf_ABYTES)
        return std::nullopt;

    const unsigned char* nonce = blob.data();
    const unsigned char* ct = blob.data() + kNonceBytes;
    unsigned long long ct_len = blob.size() - kNonceBytes;

    std::vector<uint8_t> out(ct_len);  // >= plaintext length
    unsigned long long mlen = 0;
    if (crypto_aead_xchacha20poly1305_ietf_decrypt(
            out.data(), &mlen, nullptr, ct, ct_len, nullptr, 0, nonce,
            key.data()) != 0) {
        return std::nullopt;  // wrong key / tampered / truncated
    }
    return std::string(reinterpret_cast<char*>(out.data()), mlen);
}

}  // namespace lockstep::crypto
