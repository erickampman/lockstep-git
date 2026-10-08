#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Symmetric AEAD for the rendezvous blobs: libsodium XChaCha20-Poly1305 with a
// single shared key copied out of band between the two machines (see DESIGN.md
// security model — no asymmetric crypto, no MITM surface). A fresh random nonce
// is prepended to each ciphertext.
namespace lockstep::crypto {

inline constexpr size_t kKeyBytes = 32;    // crypto_aead_xchacha20poly1305_ietf_KEYBYTES
inline constexpr size_t kNonceBytes = 24;  // ..._NPUBBYTES

using Key = std::vector<uint8_t>;  // kKeyBytes long

// Must be called once before any other function here. Returns false if
// libsodium fails to initialize (should never happen in practice).
bool init();

// Generate a fresh random key (kKeyBytes).
Key generate_key();

// Load the shared key from `path`. The file holds the key as lowercase hex
// (64 chars), optionally with trailing whitespace. Returns nullopt if the file
// is missing/unreadable or not a valid 32-byte hex key; `err` gets a message.
std::optional<Key> load_key(const std::string& path, std::string* err = nullptr);

// Write `key` to `path` as hex with mode 0600 (created, or truncated if it
// exists). Returns false on failure (and sets `err`).
bool save_key(const std::string& path, const Key& key, std::string* err = nullptr);

// Encrypt `plaintext` under `key`. Output is nonce(24) || ciphertext+tag(16),
// safe to publish to the rendezvous. Returns nullopt only on misuse (bad key).
std::optional<std::vector<uint8_t>> encrypt(const Key& key,
                                            const std::string& plaintext);

// Decrypt a blob produced by encrypt(). Returns nullopt if the key is wrong,
// the blob is truncated, or the auth tag fails (tampered) — all indistinguishable
// and all meaning "not a valid message from a key-holder".
std::optional<std::string> decrypt(const Key& key,
                                    const std::vector<uint8_t>& blob);

}  // namespace lockstep::crypto
