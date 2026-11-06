#pragma once

#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>
#include <system_error>

#include <asio/ssl/context.hpp>
#include <openssl/types.h>

#include "ruvia/web/tls_config.h"

#include "client/client_tls_config_storage.h"

namespace ruvia::detail {

enum class client_tls_setup_error : std::uint8_t {
    none,
    reset_failed,
    sni_failed,
    peer_identity_failed,
};

// Startup-only. Applies the shared TLS protocol policy (tls_protocol_policy.h);
// trust and client identity loading have one implementation.
// The storage must come from a validated client_tls_config with TLS enabled.
void configure_client_tls_context(SSL_CTX& context_value, const client_tls_config_storage& config);
// Resets the reusable SSL of a client stream and prepares SNI (DNS names only,
// one trailing dot removed) and the expected peer host or IP. A host name
// matches DNS-ID subjectAltNames only: no Common Name fallback, no
// partial-label wildcards. `host` need not be NUL-terminated.
[[nodiscard]] client_tls_setup_error prepare_client_tls_stream(SSL& ssl, std::string_view host);
// Carries a setup failure through transport results; message() names it.
[[nodiscard]] std::error_code make_client_tls_setup_error_code(client_tls_setup_error error) noexcept;

}  // namespace ruvia::detail
