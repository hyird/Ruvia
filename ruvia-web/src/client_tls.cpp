#include "client_tls.h"

#include <algorithm>
#include <array>
#include <exception>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <openssl/crypto.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include "blocking_pool.h"
#include "host_name_syntax.h"
#include "http_client_types.h"
#include "stop_token.h"
#include "task.h"
#include "tls.h"
#include "worker_ref.h"

namespace ruvia::detail {

namespace {

struct client_tls_preparation final {
    explicit client_tls_preparation(client_tls_config_storage config) :
        config_(std::move(config)),
        password_() {}
    ~client_tls_preparation() {
        if (!password_.empty()) {
            OPENSSL_cleanse(password_.data(), password_.size());
        }
    }
    client_tls_config_storage config_;
    std::string password_;
};

task<client_tls_config_storage> prepare_client_tls_owned(worker_ref worker, std::unique_ptr<client_tls_preparation> input, stop_token stop) {
    auto result = co_await run_blocking(std::move(worker), [input = std::move(input), stop = std::move(stop)]() mutable {
        if (stop.stop_requested()) {
            throw std::runtime_error("client TLS preparation cancelled");
        }
        input->config_.parse_files(input->password_);
        return std::move(input->config_);
    });
    co_return std::move(result).take_value();
}

constexpr std::array<unsigned char, 9> http11_alpn = {8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
constexpr std::array<unsigned char, 3> http2_alpn = {2, 'h', '2'};
constexpr std::array<unsigned char, 12> negotiated_http_alpn = {2, 'h', '2', 8, 'h', 't', 't', 'p', '/', '1', '.', '1'};

[[nodiscard]] bool configure_client_tls_peer_identity(SSL& connection, const char* host, bool ip_address) noexcept {
    X509_VERIFY_PARAM* const parameters = SSL_get0_param(&connection);
    if (ip_address) {
        return X509_VERIFY_PARAM_set1_host(parameters, nullptr, 0) == 1 && X509_VERIFY_PARAM_set1_ip_asc(parameters, host) == 1;
    }
    // RFC 9110 §4.3.4 / RFC 9525 §6.3: DNS-ID only, never the subject Common
    // Name, and a wildcard only as the complete left-most label.
    X509_VERIFY_PARAM_set_hostflags(parameters, X509_CHECK_FLAG_NEVER_CHECK_SUBJECT | X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
    return X509_VERIFY_PARAM_set1_ip(parameters, nullptr, 0) == 1 && X509_VERIFY_PARAM_set1_host(parameters, host, 0) == 1;
}

// Prepares SNI and the expected peer identity; TLS always authenticates the peer.
[[nodiscard]] client_tls_setup_error prepare_client_tls_session(SSL& ssl, std::string_view host) noexcept {
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
    if (!configure_client_tls_peer_identity(ssl, tls_host_buffer.data(), is_ip_address)) {
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

client_tls_config_storage::client_tls_config_storage(const client_tls_config& source_value, client_tls_files files) :
    mode_(source_value.mode_),
    minimum_version_(source_value.minimum_version_),
    max_outgoing_records_(source_value.max_outgoing_records_) {
    if (source_value.ca_file_.has_value()) {
        ca_file_.emplace(make_tls_file_name(*source_value.ca_file_));
    }
    if (source_value.identity_.has_value()) {
        identity_.emplace(make_tls_identity_files(*source_value.identity_));
    }
    if (source_value.server_name_.has_value()) {
        server_name_.emplace(*source_value.server_name_);
    }
    if (files != client_tls_files::parse || mode_ == client_tls_mode::disabled) {
        return;
    }
    parse_files(source_value.identity_ ? std::string_view(source_value.identity_->private_key_password_) : std::string_view{});
}

void client_tls_config_storage::parse_files(std::string_view private_key_password) {
    // An owner may supply its shared system trust store; it is never re-read here.
    if (!trust_store_.has_value()) {
        trust_store_ = ca_file_.has_value() ? load_tls_trust_store(*ca_file_) : tls_trust_store::load_default();
    }
    if (identity_.has_value()) {
        certificate_ = load_tls_certificate(*identity_, private_key_password);
    }
}

task<client_tls_config_storage> prepare_client_tls_config(worker_ref worker, client_tls_config_storage config, std::string_view private_key_password, stop_token stop) {
    auto input = std::make_unique<client_tls_preparation>(std::move(config));
    // Keep the secret at one stable address: moving an SSO string can leave
    // uncleansed bytes behind in an abandoned coroutine or callable frame.
    input->password_.assign(private_key_password);
    return prepare_client_tls_owned(std::move(worker), std::move(input), std::move(stop));
}

client_tls_config_storage::client_tls_config_storage(const client_tls_config_storage& source_value) :
    mode_(source_value.mode_),
    minimum_version_(source_value.minimum_version_),
    max_outgoing_records_(source_value.max_outgoing_records_),
    trust_store_(source_value.trust_store_),
    certificate_(source_value.certificate_) {
    if (source_value.ca_file_.has_value()) {
        ca_file_.emplace(*source_value.ca_file_);
    }
    if (source_value.identity_.has_value()) {
        identity_.emplace(copy_tls_identity_files(*source_value.identity_));
    }
    if (source_value.server_name_.has_value()) {
        server_name_.emplace(*source_value.server_name_);
    }
}

void configure_client_tls_context(SSL_CTX& context_value, const client_tls_config_storage& config) {
    if (config.mode_ != client_tls_mode::verify_identity) {
        throw std::invalid_argument("client TLS context requires TLS to be enabled");
    }
    apply_tls_protocol_policy(context_value, config.max_outgoing_records_);
    if (SSL_CTX_set_min_proto_version(&context_value, static_cast<int>(config.minimum_version_)) != 1) {
        throw std::runtime_error("failed to configure client TLS minimum version");
    }
    SSL_CTX_set_verify(&context_value, SSL_VERIFY_PEER, nullptr);
    if (!config.trust_store_.has_value()) {
        throw std::logic_error("client TLS trust store was not parsed");
    }
    config.trust_store_->install(context_value);
    if (!config.identity_.has_value()) {
        return;
    }
    if (!config.certificate_.has_value()) {
        throw std::logic_error("client TLS certificate was not parsed");
    }
    config.certificate_->install(context_value);
}

client_tls_setup_error prepare_client_tls_stream(SSL& ssl, std::string_view host, std::span<const unsigned char> protocols) {
    if (SSL_clear(&ssl) != 1) {
        return client_tls_setup_error::reset_failed;
    }
    if (const auto prepared = prepare_client_tls_session(ssl, host); prepared != client_tls_setup_error::none) {
        return prepared;
    }
    if (SSL_set_alpn_protos(&ssl, protocols.data(), static_cast<unsigned int>(protocols.size())) != 0) {
        return client_tls_setup_error::alpn_failed;
    }
    return client_tls_setup_error::none;
}

std::error_code make_client_tls_setup_error_code(client_tls_setup_error error) noexcept {
    static const client_tls_setup_category_type category;
    return {static_cast<int>(error), category};
}

}  // namespace ruvia::detail
