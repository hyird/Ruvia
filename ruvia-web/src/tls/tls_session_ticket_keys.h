#pragma once

#include <array>
#include <cstddef>

#include <openssl/types.h>

namespace ruvia::detail {

// One listener's TLS session ticket key set (16-byte key name, 32-byte HMAC
// key, 32-byte AES key). Generated once when the server configuration is
// validated and copied read-only into every worker's TCP and QUIC SSL_CTX, so a
// ticket issued by one worker resumes on any worker of the same listener.
class tls_session_ticket_keys final {
public:
    static constexpr std::size_t size = 80;

    [[nodiscard]] static tls_session_ticket_keys generate();

    tls_session_ticket_keys(const tls_session_ticket_keys&) noexcept = default;
    tls_session_ticket_keys& operator=(const tls_session_ticket_keys&) noexcept = default;
    ~tls_session_ticket_keys();

    // Installs this key set as the context's ticket encryption keys. Startup
    // only: the context is not yet shared with any connection.
    void install(SSL_CTX& context) const;

private:
    tls_session_ticket_keys() noexcept = default;

    std::array<unsigned char, size> bytes_{};
};

}  // namespace ruvia::detail
