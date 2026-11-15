#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include "tls_config.h"

// The transport shape every outbound client shares: HTTP, raw TCP, Redis and
// database clients configure TLS and TCP socket policy with these two structs.

namespace ruvia {

enum class client_tls_mode : std::uint8_t {
    verify_identity,
    // Plaintext.
    disabled,
};

// The one client TLS shape shared by database and Redis clients. TLS always
// authenticates the peer. Redis clients read the CA bundle and identity once,
// when the registration is created, and share them across workers; database
// drivers read the files when connecting and do not accept an encrypted
// private key. Clients default to TLS 1.2 or newer; minimum_version_ can
// require TLS 1.3. TLS 1.2 offers only ECDHE with AEAD suites, and every
// version refuses renegotiation. Every client verifies
// identities by subjectAltName type (RFC 9525): DNS names match DNS-ID entries,
// IP literals match IP entries, the subject Common Name is never used, and DNS
// wildcards cover only a complete left-most label.
struct client_tls_config final {
    client_tls_mode mode_{client_tls_mode::verify_identity};
    // Shared by HTTP, raw TCP, Redis and database clients. QUIC additionally
    // requires TLS 1.3, never lowering this one.
    tls_version minimum_version_{tls_version::tls_1_2};
    // The CA bundle that authenticates the server; nullopt selects the system
    // or driver trust store.
    std::optional<std::filesystem::path> ca_file_{};
    // The certificate this client presents for mutual TLS.
    std::optional<tls_identity_config> identity_{};
    // SNI and verified peer name, an IP literal or ASCII DNS name; nullopt
    // uses the configured host. An IP literal matches IP subjectAltNames.
    std::optional<std::string> server_name_{};
    // 1..max_outgoing_tls_records；也适用于数据库连接。
    std::uint64_t max_outgoing_records_{max_outgoing_tls_records};
};

enum class tcp_no_delay_policy : std::uint8_t {
    system_default,
    enable,
};

enum class tcp_keep_alive_policy : std::uint8_t {
    system_default,
    enable,
};

// Socket policies applied to every outbound TCP connection of one client.
// Establishment fails when an enabled policy cannot be applied.
struct client_tcp_config final {
    tcp_no_delay_policy no_delay_{tcp_no_delay_policy::enable};
    tcp_keep_alive_policy keep_alive_{tcp_keep_alive_policy::enable};
};

}  // namespace ruvia
