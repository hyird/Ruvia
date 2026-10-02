#pragma once

#include <initializer_list>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>

#include "ruvia/core/ConfigValidation.h"
#include "ruvia/web/ClientTlsConfig.h"

namespace ruvia::detail {

inline void validate_client_tls_config(const client_tls_config& config) {
    if (config.mode != client_tls_mode::disabled && config.mode != client_tls_mode::verify_identity) {
        throw std::invalid_argument("invalid client TLS mode");
    }
    if (config.certificate_file.empty() != config.private_key_file.empty()) {
        throw std::invalid_argument("client TLS certificate and private key must be configured together");
    }
    for (const std::string_view value : {std::string_view(config.ca_file), std::string_view(config.certificate_file), std::string_view(config.private_key_file), std::string_view(config.server_name)}) {
        if (value.contains('\0')) {
            throw std::invalid_argument("client TLS configuration must not contain NUL bytes");
        }
    }
    if (!config.server_name.empty()) {
        ensureConfigHost(config.server_name, "TLS server name must not be empty", "TLS server name is invalid", kSeparatedPortHostRules);
    }
    if (config.mode == client_tls_mode::disabled && (!config.ca_file.empty() || !config.certificate_file.empty() || !config.server_name.empty())) {
        throw std::invalid_argument("disabled TLS must not have TLS credentials or a server name");
    }
}

struct client_tls_config_storage final {
    template <typename Config>
    client_tls_config_storage(const Config& source, std::pmr::memory_resource* resource)
        : mode(source.mode),
          ca_file(source.ca_file, resource),
          certificate_file(source.certificate_file, resource),
          private_key_file(source.private_key_file, resource),
          server_name(source.server_name, resource) {}

    client_tls_mode mode{client_tls_mode::verify_identity};
    std::pmr::string ca_file;
    std::pmr::string certificate_file;
    std::pmr::string private_key_file;
    std::pmr::string server_name;
};

}  // namespace ruvia::detail
