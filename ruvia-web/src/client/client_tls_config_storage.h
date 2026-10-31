#pragma once

#include <cstdint>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/web/tls_config.h"

#include "client/client_host.h"
#include "tls/tls_credential.h"
#include "tls/tls_files.h"
#include "tls/tls_protocol_policy.h"

namespace ruvia::detail {

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

}  // namespace ruvia::detail
