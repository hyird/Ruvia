#pragma once

#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#include <asio/ssl/context.hpp>
#include <openssl/types.h>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/http/http_request_target.h"
#include "ruvia/web/tls_config.h"

#include "tls/dns_host.h"
#include "tls/tls_credential.h"
#include "tls/tls_files.h"
#include "tls/tls_protocol_policy.h"
#include "util/config_validation.h"
namespace ruvia::detail {

[[nodiscard]] inline bool is_client_ip_address(std::string_view host) noexcept {
    return ::ruvia::is_valid_http_ipv4_literal(host) || ::ruvia::is_valid_http_ipv6_literal(host);
}

// A client transport host, also used for TLS SNI and peer verification: an
// unbracketed IP literal or an ASCII DNS host name.
inline void validate_client_origin_host(
    std::string_view host, const char* empty_message, const char* invalid_message) {
    ensure_config_host(host, empty_message, invalid_message);
    if (!is_client_ip_address(host) && !is_valid_dns_host(host)) {
        throw std::invalid_argument(invalid_message);
    }
}

// File paths are validated when the storage is built, before any file is read.
inline void validate_client_tls_config(const client_tls_config& config) {
    validate_tls_record_limit(config.max_outgoing_records_);
    if (config.mode_ != client_tls_mode::verify_identity && config.mode_ != client_tls_mode::disabled) {
        throw std::invalid_argument("invalid client TLS mode");
    }
    if (config.server_name_.has_value()) {
        if (config.server_name_->find('\0') != std::string_view::npos) {
            throw std::invalid_argument("client TLS configuration must not contain NUL bytes");
        }
        // The override replaces the origin host for SNI and peer verification.
        validate_client_origin_host(*config.server_name_, "TLS server name must not be empty", "TLS server name is invalid");
    }
    if (config.mode_ == client_tls_mode::disabled &&
        (config.ca_file_.has_value() || config.identity_.has_value() || config.server_name_.has_value())) {
        throw std::invalid_argument("disabled TLS must not have TLS credentials or a server name");
    }
    // Database drivers read identity files themselves; every client keeps
    // the one file-based shape.
    if (config.identity_.has_value() &&
        (!config.identity_->certificate_chain_pem_.empty() || !config.identity_->private_key_pem_.empty())) {
        throw std::invalid_argument("client TLS identities must be PEM files, not in-memory PEM");
    }
}

// Whether a client storage parses its TLS files. Owners that build their own
// SSL_CTXs parse once at startup so every context shares the result; database
// drivers read the paths themselves and plaintext clients never use them.
enum class client_tls_files : std::uint8_t {
    keep_paths,
    parse,
};

// Startup-owned copy of a validated client_tls_config. The private key
// password is used only while parsing and is not retained.
struct client_tls_config_storage final {
    // Validates the file paths. With client_tls_files::parse and TLS enabled,
    // also reads the trust store and the client identity (when configured);
    // throws when either cannot be loaded.
    client_tls_config_storage(
        const client_tls_config& source_value, client_tls_files files, std::pmr::memory_resource* resource);

    // Copies share the parsed trust store and identity; no file is re-read.
    client_tls_config_storage(const client_tls_config_storage& source_value, std::pmr::memory_resource* resource);

    // SNI and verified peer name: the configured override, else the origin host.
    [[nodiscard]] std::string_view tls_host(std::string_view configured_host) const noexcept {
        return server_name_.has_value() ? std::string_view(*server_name_) : configured_host;
    }

    client_tls_mode mode_;
    std::uint64_t max_outgoing_records_;
    // nullopt selects the system or driver trust store.
    std::optional<tls_file_name> ca_file_;
    std::optional<tls_identity_files> identity_;
    std::optional<std::pmr::string> server_name_;
    // Parsed by client_tls_files::parse: the trust store and the configured
    // client identity.
    std::optional<tls_trust_store> trust_store_;
    std::optional<tls_certificate> certificate_;
};

enum class client_tls_setup_error : std::uint8_t {
    none,
    reset_failed,
    sni_failed,
    alpn_failed,
    peer_identity_failed,
    protocol_failed,
};

// The ALPN offer of one client protocol. HTTP/1 and WebSocket offer
// http/1.1; protocols without an ALPN identifier offer nothing.
enum class client_alpn : std::uint8_t {
    none,
    http11,
    http2,
    negotiate,
};
[[nodiscard]] std::span<const unsigned char> client_alpn_bytes(client_alpn mode) noexcept;

// Startup-only. Applies the shared TLS protocol policy (tls_protocol_policy.h);
// trust and client identity loading have one implementation; the ALPN offer is
// installed per connection by the TCP transport.
// The storage must come from a validated client_tls_config with TLS enabled.
void configure_client_tls_context(SSL_CTX& context_value, const client_tls_config_storage& config);
// Resets the reusable SSL of a client stream, prepares SNI (DNS names only, one
// trailing dot removed), the expected peer host or IP, and installs the ALPN
// offer. An h2-only offer requires TLS 1.2 or newer (RFC 9113).
// A host name matches DNS-ID subjectAltNames only: no Common Name
// fallback, no partial-label wildcards. `host` need not be NUL-terminated.
[[nodiscard]] client_tls_setup_error prepare_client_tls_stream(SSL& ssl, std::string_view host, std::span<const unsigned char> protocols);
// Carries a setup failure through transport results; message() names it.
[[nodiscard]] std::error_code make_client_tls_setup_error_code(client_tls_setup_error error) noexcept;

}  // namespace ruvia::detail
