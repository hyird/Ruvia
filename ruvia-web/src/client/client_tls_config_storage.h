#pragma once

#include <initializer_list>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>

#include "ruvia/core/config_validation.h"
#include "ruvia/web/client_tls_config.h"

#include "tls/tls_file_paths.h"

namespace ruvia::detail {

inline void validate_client_tls_config(const client_tls_config& config) {
    if (config.mode_ != client_tls_mode::disabled && config.mode_ != client_tls_mode::verify_identity) {
        throw std::invalid_argument("invalid client TLS mode");
    }
    if (config.certificate_file_.empty() != config.private_key_file_.empty()) {
        throw std::invalid_argument("client TLS certificate and private key must be configured together");
    }
    validate_tls_file_paths({config.ca_file_, config.certificate_file_, config.private_key_file_});
    if (config.server_name_.find('\0') != std::string_view::npos) {
        throw std::invalid_argument("client TLS configuration must not contain NUL bytes");
    }
    if (!config.server_name_.empty()) {
        ensure_config_host(config.server_name_, "TLS server name must not be empty", "TLS server name is invalid", separated_port_host_rules);
    }
    if (config.mode_ == client_tls_mode::disabled && (!config.ca_file_.empty() || !config.certificate_file_.empty() || !config.server_name_.empty())) {
        throw std::invalid_argument("disabled TLS must not have TLS credentials or a server name");
    }
}

struct client_tls_config_storage final {
    template <typename config_type>
    client_tls_config_storage(const config_type& source_value, std::pmr::memory_resource* resource)
        : mode_(source_value.mode_),
          ca_file_(source_value.ca_file_, resource),
          certificate_file_(source_value.certificate_file_, resource),
          private_key_file_(source_value.private_key_file_, resource),
          server_name_(source_value.server_name_, resource) {}

    client_tls_mode mode_{client_tls_mode::verify_identity};
    std::pmr::string ca_file_;
    std::pmr::string certificate_file_;
    std::pmr::string private_key_file_;
    std::pmr::string server_name_;
};

}  // namespace ruvia::detail
