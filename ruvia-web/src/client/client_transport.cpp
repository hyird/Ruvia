#include "client/client_transport.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <openssl/ssl.h>

#include "ruvia/core/config_validation.h"
#include "ruvia/core/dns_host.h"
#include "ruvia/core/memory/pmr_resource.h"
#include "ruvia/core/tcp_socket_options.h"
#include "ruvia/http/http_request_target.h"

#include "tls/tls_file_paths.h"
#include "tls/tls_password_scope.h"

namespace ruvia::detail {
namespace {

constexpr std::array<unsigned char, 9> http11_alpn = {8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
constexpr std::array<unsigned char, 3> http2_alpn = {2, 'h', '2'};
constexpr std::array<unsigned char, 12> negotiated_http_alpn = {
    2, 'h', '2', 8, 'h', 't', 't', 'p', '/', '1', '.', '1'};

[[nodiscard]] std::span<const unsigned char> client_alpn_bytes(client_alpn_mode mode) noexcept {
    switch (mode) {
        case client_alpn_mode::http11:
            return http11_alpn;
        case client_alpn_mode::http2:
            return http2_alpn;
        case client_alpn_mode::negotiate:
            return negotiated_http_alpn;
    }
    std::terminate();
}

}  // namespace

bool is_client_ip_address(std::string_view host) noexcept {
    return ::ruvia::is_valid_http_ipv4_literal(host) || ::ruvia::is_valid_http_ipv6_literal(host);
}

std::string_view format_client_port(std::uint16_t port, client_port_text_buffer_type& buffer) noexcept {
    const auto [end, error] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), port);
    if (error != std::errc{}) {
        std::terminate();
    }
    return {buffer.data(), static_cast<std::size_t>(end - buffer.data())};
}

std::string_view selected_client_alpn(SSL* ssl) noexcept {
    const unsigned char* selected = nullptr;
    unsigned int size = 0;
    SSL_get0_alpn_selected(ssl, &selected, &size);
    return {reinterpret_cast<const char*>(selected), size};
}

client_transport_config_storage::client_transport_config_storage(
    client_transport_config_view source_value, std::pmr::memory_resource* resource)
    : client_transport_config_storage(
          resolved_pmr_resource_tag{}, source_value, pmr_resource_or_default(resource)) {}

client_transport_config_storage::client_transport_config_storage(
    resolved_pmr_resource_tag, client_transport_config_view source_value, std::pmr::memory_resource* resource)
    : tls_peer_verification_(source_value.tls_peer_verification_),
      tcp_no_delay_(source_value.tcp_no_delay_),
      tcp_keep_alive_(source_value.tcp_keep_alive_),
      ca_file_(source_value.ca_file_, resource),
      certificate_chain_file_(source_value.certificate_chain_file_, resource),
      private_key_file_(source_value.private_key_file_, resource),
      private_key_password_(source_value.private_key_password_, resource) {}

client_transport_config_storage::client_transport_config_storage(
    const client_transport_config_storage& source_value, std::pmr::memory_resource* resource)
    : client_transport_config_storage(source_value.view(), resource) {}

client_transport_config_view client_transport_config_storage::view() const noexcept {
    return {
        .tls_peer_verification_ = tls_peer_verification_,
        .tcp_no_delay_ = tcp_no_delay_,
        .tcp_keep_alive_ = tcp_keep_alive_,
        .ca_file_ = ca_file_,
        .certificate_chain_file_ = certificate_chain_file_,
        .private_key_file_ = private_key_file_,
        .private_key_password_ = private_key_password_,
    };
}

void validate_client_origin_host(
    std::string_view host, const char* empty_message, const char* invalid_message) {
    ruvia::ensure_config_host(host, empty_message, invalid_message, ruvia::separated_port_host_rules);
    if (is_client_ip_address(host)) {
        return;
    }
    if (!ruvia::is_valid_dns_host(host)) {
        throw std::invalid_argument(invalid_message);
    }
}

std::pmr::string client_uri_host(std::string_view host, std::pmr::memory_resource* resource) {
    std::pmr::string wire_host(pmr_resource_or_default(resource));
    if ((host.find(':') != std::string_view::npos)) {
        wire_host.reserve(host.size() + 2);
        wire_host.push_back('[');
        wire_host.append(host);
        wire_host.push_back(']');
    } else {
        wire_host.assign(host);
    }
    return wire_host;
}

void validate_client_transport_config(client_transport_config_view config) {
    if (config.tls_peer_verification_ != tls_peer_verification_policy::verify &&
        config.tls_peer_verification_ != tls_peer_verification_policy::skip_verification) {
        throw std::invalid_argument("client TLS peer verification policy is invalid");
    }
    ruvia::validate_tcp_socket_policies(config.tcp_no_delay_, config.tcp_keep_alive_);
    validate_tls_file_paths({config.ca_file_, config.certificate_chain_file_, config.private_key_file_});
    if (config.certificate_chain_file_.empty() != config.private_key_file_.empty()) {
        throw std::invalid_argument(
            "client certificate chain and private key must be configured together");
    }
}

void configure_client_tls_context(SSL_CTX& context_value, client_transport_config_view config,
    client_tls_protocol protocol) {
    validate_client_transport_config(config);
    if (protocol != client_tls_protocol::stream && protocol != client_tls_protocol::quic) {
        throw std::invalid_argument("invalid client TLS protocol");
    }
    const bool quic = protocol == client_tls_protocol::quic;
    if (SSL_CTX_set_min_proto_version(&context_value, quic ? TLS1_3_VERSION : TLS1_2_VERSION) != 1 ||
        SSL_CTX_set_max_proto_version(&context_value, quic ? TLS1_3_VERSION : 0) != 1) {
        throw std::runtime_error("failed to configure client TLS versions");
    }
    const bool verify = config.tls_peer_verification_ == tls_peer_verification_policy::verify;
    SSL_CTX_set_verify(&context_value, verify ? SSL_VERIFY_PEER : SSL_VERIFY_NONE, nullptr);
    if (verify) {
        const int loaded = config.ca_file_.empty()
                               ? SSL_CTX_set_default_verify_paths(&context_value)
                               : SSL_CTX_load_verify_file(&context_value, std::pmr::string(config.ca_file_, process_resource()).c_str());
        if (loaded != 1) {
            throw std::runtime_error("failed to load client TLS trust store");
        }
    }
    if (config.certificate_chain_file_.empty()) {
        return;
    }
    const tls_password_scope password_scope(context_value, config.private_key_password_);
    if (SSL_CTX_use_certificate_chain_file(&context_value, std::pmr::string(config.certificate_chain_file_, process_resource()).c_str()) != 1) {
        throw std::runtime_error("failed to load client TLS certificate chain");
    }
    if (SSL_CTX_use_PrivateKey_file(&context_value, std::pmr::string(config.private_key_file_, process_resource()).c_str(), SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_check_private_key(&context_value) != 1) {
        throw std::runtime_error("failed to load or match client TLS private key");
    }
}

bool configure_client_tls_peer_identity(SSL& connection, const char* host, bool ip_address) noexcept {
    return SSL_set1_dnsname(&connection, ip_address ? nullptr : host) == 1 &&
           SSL_set1_ipaddr(&connection, ip_address ? host : nullptr) == 1;
}

client_tls_setup_error prepare_client_tls_stream(asio::ssl::stream<asio::ip::tcp::socket>& stream,
    const std::pmr::string& host, client_transport_config_view config, client_alpn_mode alpn_mode) {
    if (SSL_clear(stream.native_handle()) != 1) {
        return client_tls_setup_error::reset_failed;
    }
    const bool is_ip_address = is_client_ip_address(host);
    std::array<char, 254> sni_host_buffer;
    std::string_view tls_host = host;
    if (!is_ip_address && host.ends_with('.')) {
        tls_host.remove_suffix(1);
        for (std::size_t i = 0; i < tls_host.size(); ++i) {
            sni_host_buffer[i] = tls_host[i];
        }
        sni_host_buffer[tls_host.size()] = '\0';
        tls_host = std::string_view(sni_host_buffer.data(), tls_host.size());
    }
    // RFC 6066 HostName carries a DNS host_name, never an IPv4/IPv6 literal.
    // Host verification still receives IP literals so OpenSSL can validate IP
    // subjectAltName entries.
    if (!is_ip_address &&
        SSL_set_tlsext_host_name(stream.native_handle(), tls_host.data()) != 1) {
        return client_tls_setup_error::sni_failed;
    }
    if (config.tls_peer_verification_ == tls_peer_verification_policy::verify) {
        if (!configure_client_tls_peer_identity(*stream.native_handle(), tls_host.data(), is_ip_address)) {
            return client_tls_setup_error::peer_identity_failed;
        }
    }
    const auto protocols = client_alpn_bytes(alpn_mode);
    if (SSL_set_alpn_protos(stream.native_handle(), protocols.data(),
            static_cast<unsigned int>(protocols.size())) != 0) {
        return client_tls_setup_error::alpn_failed;
    }
    return client_tls_setup_error::none;
}

std::string_view client_tls_setup_error_message(client_tls_setup_error error) noexcept {
    switch (error) {
        case client_tls_setup_error::none:
            return {};
        case client_tls_setup_error::reset_failed:
            return "failed to reset TLS stream";
        case client_tls_setup_error::sni_failed:
            return "failed to set TLS SNI host";
        case client_tls_setup_error::alpn_failed:
            return "failed to configure TLS ALPN";
        case client_tls_setup_error::peer_identity_failed:
            return "failed to configure TLS peer identity";
    }
    std::terminate();
}

}  // namespace ruvia::detail
