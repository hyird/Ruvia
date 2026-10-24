#pragma once

#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>
#include <system_error>

#include <asio/ssl/context.hpp>
#include <openssl/types.h>

#include "ruvia/web/client_tls_config.h"

#include "client/client_tls_config_storage.h"

namespace ruvia::detail {

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

// Converts a validated, unbracketed transport host to the uri-host expected by
// HTTP origin/authority models. DNS and TLS continue to use the original host.
[[nodiscard]] std::pmr::string client_uri_host(
    std::string_view host, std::pmr::memory_resource* resource);
[[nodiscard]] std::string_view selected_client_alpn(SSL* ssl) noexcept;
// Startup-only. Applies the shared TLS protocol policy (tls_protocol_policy.h);
// trust and client identity loading have one implementation; the ALPN offer is
// installed per connection by the TCP transport.
// The storage must come from a validated client_tls_config with TLS enabled.
void configure_client_tls_context(SSL_CTX& context_value, const client_tls_config_storage& config);
// Resets the reusable SSL of a client stream, prepares SNI (DNS names only, one
// trailing dot removed), the expected peer host or IP when the mode verifies
// identity, and installs the ALPN offer. A host name matches DNS-ID
// subjectAltNames only: no Common Name fallback, no partial-label wildcards.
// `host` need not be NUL-terminated.
[[nodiscard]] client_tls_setup_error prepare_client_tls_stream(
    SSL& ssl, std::string_view host, client_tls_mode mode, client_alpn alpn);
// Carries a setup failure through transport results; message() names it.
[[nodiscard]] std::error_code make_client_tls_setup_error_code(client_tls_setup_error error) noexcept;

}  // namespace ruvia::detail
