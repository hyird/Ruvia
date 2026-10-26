#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace ruvia {

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
// driver trust store. HTTP, WebSocket and Redis clients read the CA bundle and
// identity once, when the client or its registration is created, and share
// them across workers; database drivers read the files when connecting.
// HTTP, WebSocket and Redis clients negotiate TLS 1.2 or 1.3, offer only ECDHE
// with AEAD suites for TLS 1.2, and refuse renegotiation. Every client verifies
// identities by subjectAltName type (RFC 9525): DNS names match DNS-ID entries,
// IP literals match IP entries, the subject Common Name is never used, and DNS
// wildcards cover only a complete left-most label.
struct client_tls_config final {
    client_tls_mode mode_{client_tls_mode::verify_identity};
    // File paths must not contain NUL bytes.
    std::string ca_file_{};
    std::string certificate_chain_file_{};
    std::string private_key_file_{};
    // Binary password bytes, including embedded NUL, are preserved.
    std::string private_key_password_{};
    // SNI and verified peer name, an IP literal or ASCII DNS name; nullopt
    // uses the configured host. An IP literal matches IP subjectAltNames.
    std::optional<std::string> server_name_{};
    // 每条 TLS 连接发出的记录总上限（含握手和 KeyUpdate），范围 1..2^23。
    // 达到上限后终止连接；更新密钥不重置计数。保守的连接级上限保证不会达到
    // RFC 9846 §5.5 的单密钥 AES-GCM 用量上限，也适用于数据库连接。
    std::uint64_t max_outgoing_records_{std::uint64_t{1} << 23};
};

}  // namespace ruvia
