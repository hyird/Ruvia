#include "client/client_tls.h"

#include <algorithm>
#include <array>
#include <exception>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include "ruvia/core/failure_report.h"
#include "ruvia/core/memory/pmr_object.h"

#include "tls/tls_protocol_policy.h"

namespace ruvia::detail {
namespace {

constexpr std::array<unsigned char, 9> http11_alpn = {8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
constexpr std::array<unsigned char, 3> http2_alpn = {2, 'h', '2'};
constexpr std::array<unsigned char, 12> negotiated_http_alpn = {
    2, 'h', '2', 8, 'h', 't', 't', 'p', '/', '1', '.', '1'};

[[nodiscard]] std::span<const unsigned char> client_alpn_bytes(client_alpn mode) noexcept {
    switch (mode) {
        case client_alpn::none:
            return {};
        case client_alpn::http11:
            return http11_alpn;
        case client_alpn::http2:
            return http2_alpn;
        case client_alpn::negotiate:
            return negotiated_http_alpn;
    }
    std::terminate();
}

[[nodiscard]] bool configure_client_tls_peer_identity(SSL& connection, const char* host, bool ip_address) noexcept {
    X509_VERIFY_PARAM* const parameters = SSL_get0_param(&connection);
    if (ip_address) {
        return X509_VERIFY_PARAM_set1_host(parameters, nullptr, 0) == 1 &&
               X509_VERIFY_PARAM_set1_ip_asc(parameters, host) == 1;
    }
    // RFC 9110 §4.3.4 / RFC 9525 §6.3: DNS-ID only, never the subject Common
    // Name, and a wildcard only as the complete left-most label.
    X509_VERIFY_PARAM_set_hostflags(parameters, X509_CHECK_FLAG_NEVER_CHECK_SUBJECT | X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
    return X509_VERIFY_PARAM_set1_ip(parameters, nullptr, 0) == 1 &&
           X509_VERIFY_PARAM_set1_host(parameters, host, 0) == 1;
}

[[nodiscard]] client_tls_setup_error prepare_client_tls_session(
    SSL& ssl, std::string_view host, client_tls_mode mode) noexcept {
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

[[nodiscard]] std::string_view client_tls_setup_error_message(client_tls_setup_error error) noexcept {
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

client_tls_config_storage::client_tls_config_storage(
    const client_tls_config& source_value, client_tls_files files, std::pmr::memory_resource* resource)
    : mode_(source_value.mode_),
      ca_file_(source_value.ca_file_, pmr_resource_or_default(resource)),
      certificate_chain_file_(source_value.certificate_chain_file_, pmr_resource_or_default(resource)),
      private_key_file_(source_value.private_key_file_, pmr_resource_or_default(resource)),
      private_key_password_(source_value.private_key_password_, pmr_resource_or_default(resource)) {
    if (source_value.server_name_.has_value()) {
        server_name_.emplace(*source_value.server_name_, pmr_resource_or_default(resource));
    }
    if (files != client_tls_files::parse || mode_ == client_tls_mode::disabled) {
        return;
    }
    if (mode_ == client_tls_mode::verify_identity) {
        trust_store_ = ca_file_.empty() ? tls_trust_store::load_default()
                                        : tls_trust_store::load_file(ca_file_.c_str());
    }
    if (!certificate_chain_file_.empty()) {
        certificate_ = tls_certificate::load(
            certificate_chain_file_.c_str(), private_key_file_.c_str(), private_key_password_);
    }
}

std::string_view selected_client_alpn(SSL* ssl) noexcept {
    const unsigned char* selected = nullptr;
    unsigned int size = 0;
    SSL_get0_alpn_selected(ssl, &selected, &size);
    return {reinterpret_cast<const char*>(selected), size};
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

void configure_client_tls_context(SSL_CTX& context_value, const client_tls_config_storage& config) {
    if (config.mode_ != client_tls_mode::verify_identity && config.mode_ != client_tls_mode::skip_verification) {
        throw std::invalid_argument("client TLS context requires TLS to be enabled");
    }
    apply_tls_protocol_policy(context_value);
    const bool verify = config.mode_ == client_tls_mode::verify_identity;
    SSL_CTX_set_verify(&context_value, verify ? SSL_VERIFY_PEER : SSL_VERIFY_NONE, nullptr);
    if (verify) {
        if (!config.trust_store_.has_value()) {
            throw std::logic_error("client TLS trust store was not parsed");
        }
        config.trust_store_->install(context_value);
    }
    if (config.certificate_chain_file_.empty()) {
        return;
    }
    if (!config.certificate_.has_value()) {
        throw std::logic_error("client TLS certificate was not parsed");
    }
    config.certificate_->install(context_value);
}

client_tls_setup_error prepare_client_tls_stream(asio::ssl::stream<asio::ip::tcp::socket>& stream,
    std::string_view host, client_tls_mode mode, client_alpn alpn) {
    if (SSL_clear(stream.native_handle()) != 1) {
        return client_tls_setup_error::reset_failed;
    }
    if (const auto prepared = prepare_client_tls_session(*stream.native_handle(), host, mode);
        prepared != client_tls_setup_error::none) {
        return prepared;
    }
    const auto protocols = client_alpn_bytes(alpn);
    if (SSL_set_alpn_protos(stream.native_handle(), protocols.data(),
            static_cast<unsigned int>(protocols.size())) != 0) {
        return client_tls_setup_error::alpn_failed;
    }
    return client_tls_setup_error::none;
}

std::error_code make_client_tls_setup_error_code(client_tls_setup_error error) noexcept {
    static const client_tls_setup_category_type category;
    return {static_cast<int>(error), category};
}

}  // namespace ruvia::detail
