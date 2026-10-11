#pragma once

#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "ruvia/core/memory/pmr_resource.h"
#include "ruvia/core/tcp_socket_options.h"
#include "ruvia/web/client_tcp_config.h"
#include "ruvia/web/client_tls_config.h"

#include "tls/tls_file_paths.h"
#include "util/config_validation.h"

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
        ensure_config_host(*config.server_name_, "TLS server name must not be empty", "TLS server name is invalid",
            separated_port_host_rules);
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

// Startup-owned copy of a validated client_tls_config.
struct client_tls_config_storage final {
    client_tls_config_storage(const client_tls_config& source_value, std::pmr::memory_resource* resource)
        : client_tls_config_storage(source_value.mode_, source_value.ca_file_, source_value.certificate_chain_file_,
              source_value.private_key_file_, source_value.private_key_password_, source_value.server_name_,
              pmr_resource_or_default(resource)) {}

    client_tls_config_storage(const client_tls_config_storage& source_value, std::pmr::memory_resource* resource)
        : client_tls_config_storage(source_value.mode_, source_value.ca_file_, source_value.certificate_chain_file_,
              source_value.private_key_file_, source_value.private_key_password_, source_value.server_name_,
              pmr_resource_or_default(resource)) {}

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

private:
    template <typename server_name_type>
    client_tls_config_storage(client_tls_mode mode, std::string_view ca_file, std::string_view certificate_chain_file,
        std::string_view private_key_file, std::string_view private_key_password,
        const std::optional<server_name_type>& server_name, std::pmr::memory_resource* resource)
        : mode_(mode),
          ca_file_(ca_file, resource),
          certificate_chain_file_(certificate_chain_file, resource),
          private_key_file_(private_key_file, resource),
          private_key_password_(private_key_password, resource) {
        if (server_name.has_value()) {
            server_name_.emplace(std::string_view(*server_name), resource);
        }
    }
};

}  // namespace ruvia::detail
