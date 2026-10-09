#pragma once

#include <array>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>

#include <asio/ip/tcp.hpp>
#include <asio/ssl/context.hpp>
#include <asio/ssl/stream.hpp>
#include <openssl/types.h>

#include "ruvia/core/memory/pmr_resource.h"
#include "ruvia/core/tcp_socket_options.h"
#include "ruvia/web/tls_peer_verification.h"

namespace ruvia::detail {

struct client_transport_config_view final {
    tls_peer_verification_policy tls_peer_verification_{tls_peer_verification_policy::verify};
    tcp_no_delay_policy tcp_no_delay_{tcp_no_delay_policy::enable};
    tcp_keep_alive_policy tcp_keep_alive_{tcp_keep_alive_policy::enable};
    std::string_view ca_file_{};
    std::string_view certificate_chain_file_{};
    std::string_view private_key_file_{};
    std::string_view private_key_password_{};
};

class client_transport_config_storage final {
public:
    client_transport_config_storage(
        client_transport_config_view source_value, std::pmr::memory_resource* resource);
    client_transport_config_storage(
        const client_transport_config_storage& source_value, std::pmr::memory_resource* resource);

    [[nodiscard]] client_transport_config_view view() const noexcept;

private:
    client_transport_config_storage(resolved_pmr_resource_tag, client_transport_config_view source_value,
        std::pmr::memory_resource* resource);

    tls_peer_verification_policy tls_peer_verification_;
    tcp_no_delay_policy tcp_no_delay_;
    tcp_keep_alive_policy tcp_keep_alive_;
    std::pmr::string ca_file_;
    std::pmr::string certificate_chain_file_;
    std::pmr::string private_key_file_;
    std::pmr::string private_key_password_;
};

template <typename config_type>
[[nodiscard]] client_transport_config_view make_client_transport_config_view(const config_type& config) noexcept {
    if constexpr (requires { config.transport_.view(); }) {
        return config.transport_.view();
    } else {
        return {
            .tls_peer_verification_ = config.tls_peer_verification_,
            .tcp_no_delay_ = config.tcp_no_delay_,
            .tcp_keep_alive_ = config.tcp_keep_alive_,
            .ca_file_ = config.ca_file_,
            .certificate_chain_file_ = config.certificate_chain_file_,
            .private_key_file_ = config.private_key_file_,
            .private_key_password_ = config.private_key_password_,
        };
    }
}

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
void validate_client_transport_config(client_transport_config_view config);
enum class client_tls_protocol : std::uint8_t { stream,
    quic };
// Startup-only. Trust and client identity loading have one implementation;
// protocol-specific ALPN and QUIC callbacks are installed by the transport.
void configure_client_tls_context(SSL_CTX& context_value, client_transport_config_view config,
    client_tls_protocol protocol = client_tls_protocol::stream);
[[nodiscard]] bool configure_client_tls_peer_identity(
    SSL& connection, const char* host, bool ip_address) noexcept;
[[nodiscard]] client_tls_setup_error prepare_client_tls_stream(
    asio::ssl::stream<asio::ip::tcp::socket>& stream, const std::pmr::string& host,
    client_transport_config_view config, client_alpn_mode alpn_mode);
[[nodiscard]] std::string_view client_tls_setup_error_message(client_tls_setup_error error) noexcept;

}  // namespace ruvia::detail
