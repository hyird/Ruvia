#pragma once

#include <cstdint>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "ruvia/core/memory/pmr_resource.h"
#include "ruvia/core/tcp_socket_options.h"
#include "ruvia/web/client_tcp_config.h"
#include "ruvia/web/client_tls_config.h"

#include "client/client_host.h"
#include "tls/tls_credential.h"
#include "tls/tls_file_paths.h"

namespace ruvia::detail {

inline void validate_client_tls_config(const client_tls_config& config) {
    if (config.mode_ != client_tls_mode::verify_identity && config.mode_ != client_tls_mode::skip_verification &&
        config.mode_ != client_tls_mode::disabled) {
        throw std::invalid_argument("invalid client TLS mode");
    }
    if (config.certificate_chain_file_.empty() != config.private_key_file_.empty()) {
        throw std::invalid_argument("client TLS certificate chain and private key must be configured together");
    }
    validate_tls_file_paths({config.ca_file_, config.certificate_chain_file_, config.private_key_file_});
    if (config.server_name_.has_value()) {
        if (config.server_name_->find('\0') != std::string_view::npos) {
            throw std::invalid_argument("client TLS configuration must not contain NUL bytes");
        }
        // The override replaces the origin host for SNI and peer verification.
        validate_client_origin_host(*config.server_name_, "TLS server name must not be empty", "TLS server name is invalid");
    }
    if (config.mode_ == client_tls_mode::disabled &&
        (!config.ca_file_.empty() || !config.certificate_chain_file_.empty() || !config.private_key_password_.empty() ||
            config.server_name_.has_value())) {
        throw std::invalid_argument("disabled TLS must not have TLS credentials or a server name");
    }
}

inline void validate_client_tcp_config(const client_tcp_config& config) {
    validate_tcp_socket_policies(config.no_delay_, config.keep_alive_);
}

// Whether a client storage parses its TLS files. Owners that build their own
// SSL_CTXs parse once at startup so every context shares the result; database
// drivers read the paths themselves and plaintext clients never use them.
enum class client_tls_files : std::uint8_t {
    keep_paths,
    parse,
};

// Startup-owned copy of a validated client_tls_config.
struct client_tls_config_storage final {
    // With client_tls_files::parse and TLS enabled, reads the trust store (when
    // the mode verifies identity) and the client identity (when configured);
    // throws std::runtime_error when either cannot be loaded.
    client_tls_config_storage(
        const client_tls_config& source_value, client_tls_files files, std::pmr::memory_resource* resource);

    // Copies share the parsed trust store and identity; no file is re-read.
    client_tls_config_storage(const client_tls_config_storage& source_value, std::pmr::memory_resource* resource)
        : mode_(source_value.mode_),
          ca_file_(source_value.ca_file_, pmr_resource_or_default(resource)),
          certificate_chain_file_(source_value.certificate_chain_file_, pmr_resource_or_default(resource)),
          private_key_file_(source_value.private_key_file_, pmr_resource_or_default(resource)),
          private_key_password_(source_value.private_key_password_, pmr_resource_or_default(resource)),
          trust_store_(source_value.trust_store_),
          certificate_(source_value.certificate_) {
        if (source_value.server_name_.has_value()) {
            server_name_.emplace(*source_value.server_name_, pmr_resource_or_default(resource));
        }
    }

    // SNI and verified peer name: the configured override, else the origin host.
    [[nodiscard]] std::string_view tls_host(std::string_view configured_host) const noexcept {
        return server_name_.has_value() ? std::string_view(*server_name_) : configured_host;
    }

    client_tls_mode mode_;
    std::pmr::string ca_file_;
    std::pmr::string certificate_chain_file_;
    std::pmr::string private_key_file_;
    std::pmr::string private_key_password_;
    std::optional<std::pmr::string> server_name_;
    // Parsed by client_tls_files::parse: the trust store of a verifying mode
    // and the configured client identity.
    std::optional<tls_trust_store> trust_store_;
    std::optional<tls_certificate> certificate_;
};

}  // namespace ruvia::detail
