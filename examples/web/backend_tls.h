#pragma once

#include <string>
#include <string_view>

#include "ruvia/web/client_tls_config.h"

#include "environment.h"

namespace example {

// Shared startup configuration for SQL and Redis examples. With prefix
// RUVIA_DB (or RUVIA_REDIS), TLS stays verified unless RUVIA_DB_TLS=false is
// explicitly selected for a trusted local server. RUVIA_DB_CA selects a CA;
// RUVIA_DB_CERT/KEY supply a client identity; RUVIA_DB_SERVER_NAME overrides
// the verified name for drivers that support it. Empty CA uses system trust.
inline ruvia::client_tls_config backend_tls(std::string_view prefix, const environment& env = environment{}) {
    const std::string base(prefix);
    return {
        .mode_ = env.get<bool>(base + "_TLS").value_or(true)
                     ? ruvia::client_tls_mode::verify_identity
                     : ruvia::client_tls_mode::disabled,
        .ca_file_ = std::string(env.get(base + "_CA").value_or("")),
        .certificate_file_ = std::string(env.get(base + "_CERT").value_or("")),
        .private_key_file_ = std::string(env.get(base + "_KEY").value_or("")),
        .server_name_ = std::string(env.get(base + "_SERVER_NAME").value_or("")),
    };
}

}  // namespace example
