#pragma once

#include <cstdint>
#include <string>

namespace ruvia {

enum class client_tls_mode : std::uint8_t { verify_identity,
    disabled };

// Authentication is mandatory when TLS is enabled. Explicitly select disabled
// only for a trusted local transport. Empty CA selects the driver's trust store.
struct client_tls_config final {
    client_tls_mode mode_{client_tls_mode::verify_identity};
    // File paths must not contain NUL bytes.
    std::string ca_file_{};
    std::string certificate_file_{};
    std::string private_key_file_{};
    // Empty verifies the configured host. Redis and PostgreSQL support overrides.
    std::string server_name_{};
};

}  // namespace ruvia
