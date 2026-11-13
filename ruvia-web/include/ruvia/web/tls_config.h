#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/move_only_function.h"
#include "ruvia/core/task.h"
#include "ruvia/core/worker_handle.h"
#include "ruvia/http/ip_address.h"

namespace ruvia {

// One certificate chain and its private key, from exactly one source: two PEM
// files, or the same PEM text in memory (the leaf certificate followed by its
// chain, and the key). Mixing the sources, or naming only half of one, throws
// std::invalid_argument. Paths must not be empty or contain NUL; they reach
// OpenSSL and the database drivers as UTF-8, so a native Windows name never
// narrows through the ANSI code page. In-memory PEM serves HTTPS listeners
// only: database, Redis and HTTP clients read identity files and reject it.
// A non-empty password decrypts the key; its binary bytes, including
// embedded NUL, are preserved. Listener configuration stages these bytes;
// starting/adding a listener, replacing identities and certificate-store
// updates parse once off-worker and cleanse their owned password/PEM copies.
struct tls_identity_config final {
    std::filesystem::path certificate_chain_file_{};
    std::filesystem::path private_key_file_{};
    std::pmr::string certificate_chain_pem_{process_resource()};
    std::pmr::string private_key_pem_{process_resource()};
    std::pmr::string private_key_password_{process_resource()};
};

using tls_certificate_id = std::uint64_t;

// Immutable ClientHello views, valid only during the selection callback.
// Wire lists are decoded into host-order uint16 protocol identifiers and ALPN
// protocol name views. Absent extensions have empty views. No OpenSSL object,
// file access or borrowed application identity is needed.
struct tls_certificate_selection final {
    std::uint64_t connection_id_{};
    ip_address remote_address_;
    std::uint16_t remote_port_{};
    std::string_view server_name_;
    std::optional<ip_address> local_address_;
    std::uint16_t local_port_{};
    bool quic_{};
    std::uint16_t legacy_version_{};
    std::span<const std::uint16_t> cipher_suites_;
    std::span<const std::uint16_t> signature_algorithms_;
    std::span<const std::uint16_t> supported_versions_;
    std::span<const std::uint16_t> supported_groups_;
    std::span<const std::string_view> alpn_;

    [[nodiscard]] bool offers_signature_algorithm(std::uint16_t algorithm) const noexcept;
};

// nullopt rejects the handshake. Exceptions and IDs absent from the snapshot
// also fail closed, including on resumed handshakes. Calls may run concurrently
// on different workers; the callable must synchronize its own mutable state.
using tls_certificate_selector = move_only_function<std::optional<tls_certificate_id>(const tls_certificate_selection&)>;

namespace detail {
struct tls_certificate_store_access;
}

// Thread-safe, independently usable certificate library. Writers serialize
// without blocking workers, prepare immutable snapshots on caller's blocking
// pool, then atomically publish on caller. Failed/rejected updates leave the
// library unchanged. Handshakes capture a snapshot before calling the selector:
// replacing/erasing its selected ID cannot invalidate that handshake.
// Established connections retain their OpenSSL references. Cold tasks retain
// the library state and own their inputs even after this object is destroyed.
// Copies are deliberately disabled; tls_config retains shared ownership.
class tls_certificate_store final {
public:
    tls_certificate_store();
    ~tls_certificate_store();
    tls_certificate_store(const tls_certificate_store&) = delete;
    tls_certificate_store& operator=(const tls_certificate_store&) = delete;

    // Await on caller; awaiting elsewhere throws std::logic_error. The caller
    // must have an attached blocking_pool for parsing or snapshot preparation.
    // Rejections throw blocking_operation_rejected; loader exceptions propagate
    // unchanged. A stopping caller cannot publish: std::runtime_error before
    // submission/after preparation, or blocking_operation_rejected while
    // waiting for the pool. Started tasks must be awaited/joined.
    // ID zero throws std::invalid_argument. Before returning a cold task, the
    // library copies secrets into process_resource storage and cleanses its
    // incoming copy, including on failure. Discarding a cold task cleanses its
    // retained copy; any detached preparation owns and cleanses its own input
    // and never publishes. These operations have no per-call timeout/token.
    [[nodiscard]] task<void> insert_or_assign(
        worker_handle caller, tls_certificate_id id, tls_identity_config identity);
    // Returns false for an absent ID without submitting blocking work.
    [[nodiscard]] task<bool> erase(worker_handle caller, tls_certificate_id id);
    [[nodiscard]] bool contains(tls_certificate_id id) const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

private:
    friend struct detail::tls_certificate_store_access;
    struct state;
    std::shared_ptr<state> state_;
};

enum class client_tls_mode : std::uint8_t {
    verify_identity,
    // Plaintext.
    disabled,
};

// TLS wire ProtocolVersion values; older versions are deliberately unsupported.
enum class tls_version : std::uint16_t {
    tls_1_1 = 0x0302,
    tls_1_2 = 0x0303,
    tls_1_3 = 0x0304,
};

// The one client TLS shape shared by database and Redis clients. TLS always
// authenticates the peer. Redis clients read the CA bundle and identity once,
// when the registration is created, and share them across workers; database
// drivers read the files when connecting and do not accept an encrypted
// private key. Clients default to TLS 1.1 or newer for legacy compatibility;
// minimum_version_ can require TLS 1.2 or 1.3. TLS 1.2 offers only ECDHE
// with AEAD suites, and every version refuses renegotiation. Every client verifies
// identities by subjectAltName type (RFC 9525): DNS names match DNS-ID entries,
// IP literals match IP entries, the subject Common Name is never used, and DNS
// wildcards cover only a complete left-most label.
struct client_tls_config final {
    client_tls_mode mode_{client_tls_mode::verify_identity};
    // Shared by HTTP, raw TCP, Redis and database clients. HTTP/2 and QUIC
    // additionally enforce their own protocol minimum, never lowering this one.
    tls_version minimum_version_{tls_version::tls_1_1};
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
    // This identity's stapled OCSP response; see tls_config::ocsp_response_.
    std::pmr::vector<std::byte> ocsp_response_{process_resource()};
};

// An additional identity served on connections that arrived at address_, a
// numeric IP address that is not unspecified (an IPv4-mapped IPv6 address
// equals its IPv4 form), when the client sent no server_name or one matching
// no sni_ entry -- typically a listener bound to a wildcard address serving
// clients that connect by IP literal, which RFC 6066 forbids naming in SNI.
// Both TLS over TCP and HTTP/3 use the actual destination address, including
// wildcard listeners; QUIC retains packet destination/interface information
// when handing datagrams between workers and selecting the reply source.
struct tls_local_address_config final {
    std::pmr::string address_{process_resource()};
    tls_identity_config identity_{};
    // This identity's stapled OCSP response; see tls_config::ocsp_response_.
    std::pmr::vector<std::byte> ocsp_response_{process_resource()};
};

// One HTTPS listener's identities. The listener negotiates TLS 1.1, 1.2 or 1.3;
// its TLS 1.2 suites are ECDHE with AEAD only, chosen by server preference;
// renegotiation is refused; ALPN selects h2, http/1.1 or http/1.0, with HTTP/2
// requiring TLS 1.2 or newer. The paired HTTP/3 service (listen_config::http3_)
// uses the same identities over QUIC with TLS 1.3 only and ALPN h3 only.
// Without certificate_store_, each handshake selects once, when its
// ClientHello is processed: an exact sni_ host, else a
// wildcard sni_ host, else a local_addresses_ entry, else the primary
// identity_. A client offering no server_name, or one matching nothing, is
// thus still served: a product choice for single-certificate and wildcard
// deployments, deliberately not RFC 9325 §3.7's unrecognized_name alert.
// Sessions resume across workers of the listener, in every supported TLS
// version only under the server_name (compared ASCII case-insensitively) they
// were established with, and across application::replace_tls_identities(). Ticket
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
    // The DER OCSPResponse (RFC 6960 §4.2.1) stapled for identity_ (RFC 6066
    // §8 status_request; in TLS 1.3 the leaf's CertificateEntry extension,
    // RFC 8446 §4.4.2.1), sent only to a client that requests OCSP status and
    // only with the identity it belongs to; each sni_ and local_addresses_
    // entry carries its own. Empty staples nothing. Invalid DER (including
    // trailing bytes or BER) throws std::invalid_argument when identities
    // load. The bytes are served as given; the caller owns certificate
    // association, signature and freshness checks. No fetching or refresh:
    // replace_tls_identities() installs a renewed response with its identity.
    // Not available with certificate_store_.
    std::pmr::vector<std::byte> ocsp_response_{process_resource()};
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
    // Optional live library, shared by every worker and both TLS transports.
    // With a library, fixed identity_/sni_/local_addresses_ must be empty.
    // Without a selector, default_certificate_ selects an ID. A selector must
    // be paired with a library and completely replaces fixed selection.
    // Updating the library never rebuilds contexts, ticket keys or caches.
    std::shared_ptr<tls_certificate_store> certificate_store_;
    std::shared_ptr<const tls_certificate_selector> select_certificate_;
    tls_certificate_id default_certificate_{};
};

}  // namespace ruvia
