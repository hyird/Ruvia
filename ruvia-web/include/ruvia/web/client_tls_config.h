#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace ruvia {

enum class client_tls_mode : std::uint8_t {
    verify_identity,
    // Encrypts without authenticating the peer. Only for trusted test or
    // local transports; features that rely on an authenticated origin reject it.
    skip_verification,
    // Plaintext. HTTP and WebSocket clients select plaintext through their
    // scheme instead and reject this mode for https/wss.
    disabled,
};

// The one client TLS shape shared by HTTP, WebSocket, database and Redis
// clients. Authentication is the default. Empty CA selects the system or
// driver trust store.
struct client_tls_config final {
    client_tls_mode mode_{client_tls_mode::verify_identity};
    // File paths must not contain NUL bytes.
    std::string ca_file_{};
    std::string certificate_chain_file_{};
    std::string private_key_file_{};
    // Binary password bytes, including embedded NUL, are preserved.
    std::string private_key_password_{};
    // SNI and verified peer name; nullopt uses the configured host.
    std::optional<std::string> server_name_{};
};

}  // namespace ruvia
