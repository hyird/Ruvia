#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>

#include <openssl/types.h>

namespace ruvia::detail {

// RFC 9325 §3.4: ticket encryption keys change regularly and are destroyed at
// the end of their validity. A key encrypts tickets for one rotation interval,
// still decrypts them for the next, and is then destroyed. The interval equals
// the server session lifetime (also the TLS 1.3 ticket lifetime and TLS 1.2
// lifetime hint), so every ticket stays decryptable for its whole lifetime and
// no key outlives two intervals.
inline constexpr std::chrono::seconds tls_session_lifetime{std::chrono::hours{2}};

// One ticket key: 16-byte key name, HMAC-SHA256 key and AES-256-CBC key, the
// protection OpenSSL applies to its own tickets. Cleansed on destruction.
struct tls_session_ticket_key final {
    tls_session_ticket_key() noexcept = default;
    tls_session_ticket_key(const tls_session_ticket_key&) noexcept = default;
    tls_session_ticket_key& operator=(const tls_session_ticket_key&) noexcept = default;
    ~tls_session_ticket_key();

    std::array<unsigned char, 16> name_{};
    std::array<unsigned char, 32> hmac_key_{};
    std::array<unsigned char, 32> aes_key_{};
};

class tls_session_ticket_key_set;

// One TLS listener's authoritative ticket keys, shared by every worker so a
// ticket issued by one worker resumes on any worker of the same listener.
// Epochs are counted from a steady-clock origin; the first worker to reach a
// new epoch generates its key under the mutex. Handshakes never touch this
// object: each worker copies the keys into its own set once per epoch.
class tls_session_ticket_key_ring final {
public:
    // Generates the key of the epoch starting at `origin`; throws when the
    // random generator fails.
    explicit tls_session_ticket_key_ring(std::chrono::steady_clock::time_point origin);

    tls_session_ticket_key_ring(const tls_session_ticket_key_ring&) = delete;
    tls_session_ticket_key_ring& operator=(const tls_session_ticket_key_ring&) = delete;

    // Any thread. Advances to the epoch containing `now`, destroying keys
    // whose validity ended, and copies the keys still valid into `target`.
    // Returns false when the new epoch's key could not be generated: `target`
    // then issues no tickets and only accepts the previous key, and a later
    // call retries the generation.
    [[nodiscard]] bool copy_to(std::chrono::steady_clock::time_point now, tls_session_ticket_key_set& target);

    // When the epoch containing `now` ends.
    [[nodiscard]] std::chrono::steady_clock::time_point next_rotation(
        std::chrono::steady_clock::time_point now) const noexcept;

private:
    [[nodiscard]] std::uint64_t epoch_at(std::chrono::steady_clock::time_point now) const noexcept;

    const std::chrono::steady_clock::time_point origin_;
    std::mutex mutex_;
    // Guarded by mutex_.
    std::uint64_t epoch_{0};
    std::optional<tls_session_ticket_key> current_;
    std::optional<tls_session_ticket_key> previous_;
};

// One worker's copy of a listener's ticket keys, installed as the ticket key
// callback of every TLS context of that listener. Only the owning worker
// thread reads it (inside handshakes) and refreshes it (between handshakes),
// so the handshake path takes no lock and touches no shared state.
class tls_session_ticket_key_set final {
public:
    tls_session_ticket_key_set();
    ~tls_session_ticket_key_set();

    tls_session_ticket_key_set(const tls_session_ticket_key_set&) = delete;
    tls_session_ticket_key_set& operator=(const tls_session_ticket_key_set&) = delete;

    // Startup only, before `context` serves a connection: issues and accepts
    // tickets with this set's keys and sets the matching session lifetime.
    void install(SSL_CTX& context);

private:
    friend class tls_session_ticket_key_ring;

    static int protect_ticket(SSL* ssl, unsigned char* key_name, unsigned char* iv, EVP_CIPHER_CTX* cipher,
        EVP_MAC_CTX* mac, int encrypt) noexcept;

    EVP_CIPHER* aes_256_cbc_;
    std::optional<tls_session_ticket_key> current_;
    std::optional<tls_session_ticket_key> previous_;
};

}  // namespace ruvia::detail
