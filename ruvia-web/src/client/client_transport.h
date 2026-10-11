#pragma once

#include <array>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <string>
#include <string_view>

#include <asio/ip/tcp.hpp>
#include <asio/ssl/context.hpp>
#include <asio/ssl/stream.hpp>
#include <openssl/types.h>

#include "ruvia/core/memory/pmr_resource.h"
#include "ruvia/web/client_tls_config.h"

#include "client/client_tls_config_storage.h"

namespace ruvia::detail {

enum class client_alpn_mode : std::uint8_t {
    http11,
    http2,
    negotiate,
};

enum class client_tls_setup_error : std::uint8_t {
    none,
    reset_failed,
    sni_failed,
    alpn_failed,
    peer_identity_failed,
};

using client_port_text_buffer_type = std::array<char, std::numeric_limits<std::uint16_t>::digits10 + 1>;

void validate_client_origin_host(
    std::string_view host, const char* empty_message, const char* invalid_message);
// Converts a validated, unbracketed transport host to the uri-host expected by
// HTTP origin/authority models. DNS and TLS continue to use the original host.
[[nodiscard]] std::pmr::string client_uri_host(
    std::string_view host, std::pmr::memory_resource* resource);
[[nodiscard]] bool is_client_ip_address(std::string_view host) noexcept;
[[nodiscard]] std::string_view format_client_port(
    std::uint16_t port, client_port_text_buffer_type& buffer) noexcept;
[[nodiscard]] std::string_view selected_client_alpn(SSL* ssl) noexcept;
enum class client_tls_protocol : std::uint8_t { stream,
    quic };
// Startup-only. Trust and client identity loading have one implementation;
// protocol-specific ALPN and QUIC callbacks are installed by the transport.
// The storage must come from a validated client_tls_config with TLS enabled.
void configure_client_tls_context(SSL_CTX& context_value, const client_tls_config_storage& config,
    client_tls_protocol protocol = client_tls_protocol::stream);
[[nodiscard]] bool configure_client_tls_peer_identity(
    SSL& connection, const char* host, bool ip_address) noexcept;
// Per-connection SNI (DNS names only, one trailing dot removed) and, when the
// mode verifies identity, the expected peer host or IP. `host` need not be
// NUL-terminated.
[[nodiscard]] client_tls_setup_error prepare_client_tls_session(
    SSL& ssl, std::string_view host, client_tls_mode mode) noexcept;
// Resets a reusable stream, prepares its session and installs HTTP ALPN.
[[nodiscard]] client_tls_setup_error prepare_client_tls_stream(asio::ssl::stream<asio::ip::tcp::socket>& stream,
    std::string_view host, client_tls_mode mode, client_alpn_mode alpn_mode);
[[nodiscard]] std::string_view client_tls_setup_error_message(client_tls_setup_error error) noexcept;
// Carries a setup failure through transport results; message() names it.
[[nodiscard]] std::error_code make_client_tls_setup_error_code(client_tls_setup_error error) noexcept;

}  // namespace ruvia::detail
