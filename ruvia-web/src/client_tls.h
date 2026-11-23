#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#include <openssl/types.h>

#include "borrowed_text.h"
#include "client_host_resolver.h"
#include "http_client_types.h"
#include "operation_options.h"
#include "stop_token.h"
#include "task.h"
#include "tls.h"
#include "tls_config.h"
#include "worker_ref.h"

namespace ruvia::detail {

// File paths are validated when the storage is built, before any file is read.
inline void validate_client_tls_config(const client_tls_config& config) {
    validate_tls_record_limit(config.max_outgoing_records_);
    if (config.mode_ != client_tls_mode::verify_identity && config.mode_ != client_tls_mode::disabled) {
        throw std::invalid_argument("invalid client TLS mode");
    }
    switch (config.minimum_version_) {
        case tls_version::tls_1_2:
        case tls_version::tls_1_3:
            break;
        default:
            throw std::invalid_argument("invalid client TLS minimum version");
    }
    if (config.server_name_.has_value()) {
        if (config.server_name_->find('\0') != std::string_view::npos) {
            throw std::invalid_argument("client TLS configuration must not contain NUL bytes");
        }
        // The override replaces the origin host for SNI and peer verification.
        validate_client_origin_host(*config.server_name_, "TLS server name must not be empty", "TLS server name is invalid");
    }
    if (config.ca_file_.has_value() && !config.ca_pem_.empty()) {
        throw std::invalid_argument("client TLS must not specify both CA file and CA PEM");
    }
    if (config.mode_ == client_tls_mode::disabled && (config.ca_file_.has_value() || !config.ca_pem_.empty() || config.identity_.has_value() || config.server_name_.has_value())) {
        throw std::invalid_argument("disabled TLS must not have TLS credentials or a server name");
    }
    if (config.identity_.has_value()) {
        const auto& identity = *config.identity_;
        // A staple belongs to a server identity (RFC 6066 §8); a client never sends one.
        if (!identity.ocsp_response_.empty()) {
            throw std::invalid_argument("client TLS identities must not carry an OCSP response");
        }
        const bool has_files = !identity.certificate_chain_file_.empty() || !identity.private_key_file_.empty();
        const bool has_pem = !identity.certificate_chain_pem_.empty() || !identity.private_key_pem_.empty();
        if (has_files && has_pem) {
            throw std::invalid_argument("client TLS identity must not mix file and PEM sources");
        }
        if (!has_files && !has_pem) {
            throw std::invalid_argument("client TLS identity must specify certificate chain and private key");
        }
        if (has_files && (identity.certificate_chain_file_.empty() || identity.private_key_file_.empty())) {
            throw std::invalid_argument("client TLS file identity requires both certificate chain and private key");
        }
        if (has_pem && (identity.certificate_chain_pem_.empty() || identity.private_key_pem_.empty())) {
            throw std::invalid_argument("client TLS PEM identity requires both certificate chain and private key");
        }
    }
}

struct client_tls_material;

// Owns validated paths and the password without reading files. Copies share
// preparation and its parsed credentials; secrets are wiped after parsing.
struct client_tls_config_storage final {
    explicit client_tls_config_storage(const client_tls_config& source_value);

    // Copies share the parsed trust store and identity; no file is re-read.
    client_tls_config_storage(const client_tls_config_storage& source_value);
    client_tls_config_storage(client_tls_config_storage&&) noexcept = default;
    client_tls_config_storage& operator=(client_tls_config_storage&&) noexcept = default;

    // SNI and verified peer name: the configured override, else the origin host.
    [[nodiscard]] std::string_view tls_host(std::string_view configured_host RUVIA_LIFETIMEBOUND) const& noexcept RUVIA_LIFETIMEBOUND {
        return server_name_.has_value() ? std::string_view(*server_name_) : configured_host;
    }
    std::string_view tls_host(std::string_view) const&& = delete;

    template <http_temporary_owning_char_string host_type>
    std::string_view tls_host(host_type&&) const& = delete;

    // True when preparing credentials reads files or walks the system trust directory.
    [[nodiscard]] bool requires_filesystem_io() const noexcept;

    client_tls_mode mode_;
    tls_version minimum_version_;
    std::uint64_t max_outgoing_records_;
    // nullopt selects ca_pem_ when non-empty, else the system or driver trust store.
    std::optional<tls_file_name> ca_file_;
    std::optional<tls_identity_files> identity_;
    bool has_identity_{false};
    std::optional<std::string> server_name_;
    std::optional<tls_trust_store> trust_store_;
    std::optional<tls_certificate> certificate_;

private:
    friend task<client_tls_config_storage> prepare_client_tls_config(worker_ref worker, client_tls_config_storage config, operation_options options);

    std::shared_ptr<client_tls_material> material_;
};

// The one credential preparation of every TLS client. Copies of a storage
// share one parse across workers: settled credentials, or the configuration's
// failure, answer without offloading. The first caller of an idle material
// offloads the file read and joins it, even after its own cancellation or
// deadline, and that read settles the material for every caller. Callers
// arriving meanwhile wait for it within their own stop token and deadline,
// holding no blocking-pool admission. A rejected offload settles nothing.
// The caller must check its host's state before publishing the result.
[[nodiscard]] task<client_tls_config_storage> prepare_client_tls_config(worker_ref worker, client_tls_config_storage config, operation_options options);

enum class client_tls_setup_error : std::uint8_t {
    none,
    reset_failed,
    sni_failed,
    alpn_failed,
    peer_identity_failed,
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

// Startup-only. Applies the shared TLS protocol policy (apply_tls_protocol_policy, tls.h);
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
