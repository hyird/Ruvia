#pragma once

#include <cstdint>
#include <filesystem>
#include <memory_resource>
#include <optional>
#include <string>
#include <vector>

#include "ruvia/core/memory/pmr_object.h"

namespace ruvia {

// One certificate chain and its private key, from exactly one source: two PEM
// files, or the same PEM text in memory (the leaf certificate followed by its
// chain, and the key). Mixing the sources, or naming only half of one, throws
// std::invalid_argument. Paths must not be empty or contain NUL; they reach
// OpenSSL and the database drivers as UTF-8, so a native Windows name never
// narrows through the ANSI code page. In-memory PEM serves HTTPS listeners
// only: database, Redis and HTTP clients read identity files and reject it.
// A non-empty password decrypts the key; its binary bytes, including
// embedded NUL, are preserved. application::listen() and
// replace_tls_identities() parse the identity once and cleanse their own
// copies of the password and of the in-memory PEM text before returning.
struct tls_identity_config final {
    std::filesystem::path certificate_chain_file_{};
    std::filesystem::path private_key_file_{};
    std::pmr::string certificate_chain_pem_{process_resource()};
    std::pmr::string private_key_pem_{process_resource()};
    std::pmr::string private_key_password_{process_resource()};
};

enum class client_tls_mode : std::uint8_t {
    verify_identity,
    // Plaintext.
    disabled,
};

// The one client TLS shape shared by database and Redis clients. TLS always
// authenticates the peer. Redis clients read the CA bundle and identity once,
// when the registration is created, and share them across workers; database
// drivers read the files when connecting and do not accept an encrypted
// private key. Redis clients negotiate TLS 1.2 or 1.3, offer only ECDHE with
// AEAD suites for TLS 1.2, and refuse renegotiation. Every client verifies
// identities by subjectAltName type (RFC 9525): DNS names match DNS-ID entries,
// IP literals match IP entries, the subject Common Name is never used, and DNS
// wildcards cover only a complete left-most label.
struct client_tls_config final {
    client_tls_mode mode_{client_tls_mode::verify_identity};
    // The CA bundle that authenticates the server; nullopt selects the system
    // or driver trust store.
    std::optional<std::filesystem::path> ca_file_{};
    // The certificate this client presents for mutual TLS.
    std::optional<tls_identity_config> identity_{};
    // SNI and verified peer name, an IP literal or ASCII DNS name; nullopt
    // uses the configured host. An IP literal matches IP subjectAltNames.
    std::optional<std::pmr::string> server_name_{};
    // 每条 TLS 连接发出的记录总上限（含握手和 KeyUpdate），范围 1..2^23。
    // 达到上限后终止连接；更新密钥不重置计数。保守的连接级上限保证不会达到
    // RFC 9846 §5.5 的单密钥 AES-GCM 用量上限，也适用于数据库连接。
    std::uint64_t max_outgoing_records_{std::uint64_t{1} << 23};
};

enum class tls_client_certificate_requirement : std::uint8_t {
    optional,
    required,
};

struct tls_client_certificate_config final {
    // The CA bundle that verifies presented client certificates. Optional mode
    // admits a client without a certificate; required mode rejects it.
    std::optional<std::filesystem::path> ca_file_{};
    tls_client_certificate_requirement requirement_{tls_client_certificate_requirement::optional};
};

// An additional identity served when the client's server_name (RFC 6066 SNI)
// equals host_, compared ASCII case-insensitively. A host_ "*.example.com" is
// a wildcard covering exactly one whole left-most label (RFC 6125 §6.4.3):
// "a.example.com", but neither "example.com" nor "a.b.example.com". An exact
// host_ wins over a wildcard covering the same name.
struct tls_sni_config final {
    std::pmr::string host_{process_resource()};
    tls_identity_config identity_{};
};

// An additional identity served on connections that arrived at address_, a
// numeric IP address that is not unspecified (an IPv4-mapped IPv6 address
// equals its IPv4 form), when the client sent no server_name or one matching
// no sni_ entry -- typically a listener bound to a wildcard address serving
// clients that connect by IP literal, which RFC 6066 forbids naming in SNI.
// TLS over TCP matches the address the client connected to. HTTP/3 knows only
// the address its UDP socket is bound to, so its connections match only on a
// listener bound to that specific address.
struct tls_local_address_config final {
    std::pmr::string address_{process_resource()};
    tls_identity_config identity_{};
};

// One HTTPS listener's identities. The listener negotiates TLS 1.2 or 1.3; its
// TLS 1.2 suites are ECDHE with AEAD only, chosen by server preference;
// renegotiation is refused; ALPN selects h2, http/1.1 or http/1.0. The paired
// HTTP/3 service (listen_config::http3_) uses the same identities over QUIC
// with TLS 1.3 only and ALPN h3 only. Each handshake selects its identity
// once, when its ClientHello is processed: an exact sni_ host, else a
// wildcard sni_ host, else a local_addresses_ entry, else the primary
// identity_. A client offering no server_name, or one matching nothing, is
// thus still served: a product choice for single-certificate and wildcard
// deployments, deliberately not RFC 9325 §3.7's unrecognized_name alert.
// Sessions resume across workers of the listener, in both TLS versions only
// under the server_name (compared ASCII case-insensitively) they were
// established with, and across application::replace_tls_identities(). Ticket
// keys rotate every two hours (the session lifetime) and are destroyed one
// interval after their last use for new tickets. RFC 9110 §7.4: a request
// whose target is an "https" resource is answered 421 (Misdirected Request)
// unless the certificate this connection presented names its host in a
// subjectAltName DNS or IP entry (whole-label wildcards only; the subject CN
// is ignored); on HTTP/2 only that stream is refused. Cleartext listeners
// answer 421 to "https" targets. Peers in trusted_proxy_config are exempt
// from both checks.
struct tls_config final {
    tls_identity_config identity_{};
    tls_client_certificate_config client_certificates_{};
    std::pmr::vector<tls_sni_config> sni_{process_resource()};
    std::pmr::vector<tls_local_address_config> local_addresses_{process_resource()};
    // 发出的 TLS 记录总上限，范围 1..2^23；含握手及控制记录，KeyUpdate 不
    // 重置计数。达到上限后终止连接，以满足 RFC 9846 §5.5 的密钥用量约束。
    std::uint64_t max_outgoing_records_{std::uint64_t{1} << 23};
    // The ALPN protocols (RFC 7301) a listener_dispatch::connection listener
    // offers, in server preference order; empty offers h2, then http/1.1.
    // Each name is 1..255 bytes. A client offering ALPN with none of them
    // fails the handshake with no_application_protocol (RFC 7301 §3.2); one
    // offering no ALPN completes it without a protocol. A routes listener
    // negotiates its own HTTP versions and rejects a non-empty list.
    std::pmr::vector<std::pmr::string> alpn_{process_resource()};
};

}  // namespace ruvia
