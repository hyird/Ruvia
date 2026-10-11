#include "client/client_transport.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#include <openssl/ssl.h>

#include "ruvia/core/memory/pmr_resource.h"
#include "ruvia/core/tcp_socket_options.h"
#include "ruvia/http/http_request_target.h"

#include "tls/tls_file_paths.h"
#include "tls/tls_password_scope.h"
#include "util/config_validation.h"
#include "util/dns_host.h"

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

void validate_client_origin_host(
    std::string_view host, const char* empty_message, const char* invalid_message) {
    detail::ensure_config_host(host, empty_message, invalid_message, detail::separated_port_host_rules);
    if (is_client_ip_address(host)) {
        return;
    }
    if (!detail::is_valid_dns_host(host)) {
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

void configure_client_tls_context(SSL_CTX& context_value, const client_tls_config_storage& config,
    client_tls_protocol protocol) {
    if (config.mode_ != client_tls_mode::verify_identity && config.mode_ != client_tls_mode::skip_verification) {
        throw std::invalid_argument("client TLS context requires TLS to be enabled");
    }
    if (protocol != client_tls_protocol::stream && protocol != client_tls_protocol::quic) {
        throw std::invalid_argument("invalid client TLS protocol");
    }
    const bool quic = protocol == client_tls_protocol::quic;
    if (SSL_CTX_set_min_proto_version(&context_value, quic ? TLS1_3_VERSION : TLS1_2_VERSION) != 1 ||
        SSL_CTX_set_max_proto_version(&context_value, quic ? TLS1_3_VERSION : 0) != 1) {
        throw std::runtime_error("failed to configure client TLS versions");
    }
    const bool verify = config.mode_ == client_tls_mode::verify_identity;
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
    X509_VERIFY_PARAM* const parameters = SSL_get0_param(&connection);
    if (ip_address) {
        return X509_VERIFY_PARAM_set1_host(parameters, nullptr, 0) == 1 &&
               X509_VERIFY_PARAM_set1_ip_asc(parameters, host) == 1;
    }
    return X509_VERIFY_PARAM_set1_ip(parameters, nullptr, 0) == 1 &&
           X509_VERIFY_PARAM_set1_host(parameters, host, 0) == 1;
}

client_tls_setup_error prepare_client_tls_session(SSL& ssl, std::string_view host, client_tls_mode mode) noexcept {
    const bool is_ip_address = is_client_ip_address(host);
    if (!is_ip_address && host.ends_with('.')) {
        host.remove_suffix(1);
    }
    std::array<char, 256> tls_host_buffer;
    if (host.size() >= tls_host_buffer.size()) {
        return client_tls_setup_error::sni_failed;
    }
    std::ranges::copy(host, tls_host_buffer.begin());
    tls_host_buffer[host.size()] = '\0';
    // RFC 6066 HostName carries a DNS host_name, never an IPv4/IPv6 literal.
    // Host verification still receives IP literals so OpenSSL can validate IP
    // subjectAltName entries.
    if (!is_ip_address && SSL_set_tlsext_host_name(&ssl, tls_host_buffer.data()) != 1) {
        return client_tls_setup_error::sni_failed;
    }
    if (mode == client_tls_mode::verify_identity &&
        !configure_client_tls_peer_identity(ssl, tls_host_buffer.data(), is_ip_address)) {
        return client_tls_setup_error::peer_identity_failed;
    }
    return client_tls_setup_error::none;
}

client_tls_setup_error prepare_client_tls_stream(asio::ssl::stream<asio::ip::tcp::socket>& stream,
    std::string_view host, client_tls_mode mode, client_alpn_mode alpn_mode) {
    if (SSL_clear(stream.native_handle()) != 1) {
        return client_tls_setup_error::reset_failed;
    }
    if (const auto prepared = prepare_client_tls_session(*stream.native_handle(), host, mode);
        prepared != client_tls_setup_error::none) {
        return prepared;
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

namespace {

class client_tls_setup_category_type final : public std::error_category {
public:
    [[nodiscard]] const char* name() const noexcept override {
        return "client TLS setup";
    }
    [[nodiscard]] std::string message(int value) const override {
        return std::string(client_tls_setup_error_message(static_cast<client_tls_setup_error>(value)));
    }
};

}  // namespace

std::error_code make_client_tls_setup_error_code(client_tls_setup_error error) noexcept {
    static const client_tls_setup_category_type category;
    return {static_cast<int>(error), category};
}

}  // namespace ruvia::detail
