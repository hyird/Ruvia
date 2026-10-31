#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "ruvia/web/tls_config.h"

#include "environment.h"

namespace example {

// Shared startup configuration for SQL and Redis examples. With prefix
// RUVIA_DB (or RUVIA_REDIS), TLS stays verified unless RUVIA_DB_TLS=false is
// explicitly selected for a trusted local server. RUVIA_DB_CA selects a CA;
// RUVIA_DB_CERT/KEY supply a client identity; RUVIA_DB_SERVER_NAME overrides
// the verified name for drivers that support it. An unset CA uses system trust.
inline ruvia::client_tls_config backend_tls(std::string_view prefix, const environment& env = environment{}) {
    const std::string base(prefix);
    ruvia::client_tls_config config{
        .mode_ = env.get<bool>(base + "_TLS").value_or(true)
                     ? ruvia::client_tls_mode::verify_identity
                     : ruvia::client_tls_mode::disabled,
        .ca_file_ = env.path(base + "_CA"),
    };
    const auto certificate = env.path(base + "_CERT");
    const auto key = env.path(base + "_KEY");
    if (certificate || key) {
        // Only one of the two is a configuration error, reported by the client.
        config.identity_ = ruvia::tls_identity_config{
            .certificate_chain_file_ = certificate.value_or(std::filesystem::path{}),
            .private_key_file_ = key.value_or(std::filesystem::path{}),
        };
    }
    if (auto name = env.get(base + "_SERVER_NAME"); name && !name->empty()) {
        config.server_name_ = std::string(*name);
    }
    return config;
}

}  // namespace example
