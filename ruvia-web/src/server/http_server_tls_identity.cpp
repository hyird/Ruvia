#include "server/http_server_tls_identity.h"

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <asio/ssl/verify_mode.hpp>
#include <openssl/err.h>
#include <openssl/ssl.h>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/http/http_ascii.h"

#include "tls/tls_credential.h"
#include "tls/tls_files.h"
#include "tls/tls_protocol_policy.h"
#include "util/dns_host.h"

namespace ruvia::detail {
namespace {

[[nodiscard]] bool is_valid_sni_host(std::string_view host) noexcept {
    return !host.empty() && !host.ends_with('.') && detail::is_valid_dns_host(host);
}

void ensure_sni_host(std::string_view host) {
    if (host.empty()) {
        throw std::invalid_argument("SNI host must not be empty");
    }
    if (!is_valid_sni_host(host)) {
        throw std::invalid_argument("SNI host is invalid");
    }
}

[[nodiscard]] asio::ssl::verify_mode http_server_tls_verify_mode(
    tls_client_certificate_requirement requirement) noexcept {
    auto mode = asio::ssl::verify_peer;
    if (requirement == tls_client_certificate_requirement::required) {
        mode |= asio::ssl::verify_fail_if_no_peer_cert;
    }
    return mode;
}

}  // namespace

http_server_listener_definition::tls load_http_server_tls(
    const tls_config& config, std::pmr::memory_resource* resource) {
    auto* const target_resource = pmr_resource_or_default(resource);
    validate_tls_record_limit(config.max_outgoing_records_);
    // Every setting is validated before any file is read.
    const auto primary = make_tls_identity_files(config.identity_, target_resource);
    const auto& client = config.client_certificates_;
    std::optional<tls_file_name> client_ca;
    if (client.ca_file_.has_value() || client.requirement_ != tls_client_certificate_requirement::optional) {
        switch (client.requirement_) {
            case tls_client_certificate_requirement::optional:
            case tls_client_certificate_requirement::required:
                break;
            default:
                throw std::invalid_argument("TLS client certificate requirement is invalid");
        }
        if (!client.ca_file_.has_value()) {
            throw std::invalid_argument("TLS client certificates require a CA bundle");
        }
        client_ca.emplace(make_tls_file_name(*client.ca_file_, target_resource));
    }
    std::pmr::vector<tls_identity_files> sni_files(target_resource);
    sni_files.reserve(config.sni_.size());
    for (std::size_t i = 0; i < config.sni_.size(); ++i) {
        const auto& sni = config.sni_[i];
        ensure_sni_host(sni.host_);
        sni_files.push_back(make_tls_identity_files(sni.identity_, target_resource));
        for (std::size_t j = 0; j < i; ++j) {
            if (http_ascii_equals_ignore_case(config.sni_[j].host_, sni.host_)) {
                throw std::invalid_argument("SNI hosts must be unique");
            }
        }
    }

    http_server_listener_definition::tls tls(
        load_tls_certificate(primary, config.identity_.private_key_password_), target_resource);
    tls.max_outgoing_records_ = config.max_outgoing_records_;
    if (client_ca.has_value()) {
        tls.client_certificates_.emplace(http_server_listener_definition::tls_client_certificate_policy{
            .requirement_ = client.requirement_,
            .client_ca_ = load_tls_trust_store(*client_ca),
        });
    }
    tls.sni_identities_.reserve(config.sni_.size());
    for (std::size_t i = 0; i < config.sni_.size(); ++i) {
        const auto& sni = config.sni_[i];
        tls.sni_identities_.emplace_back(
            sni.host_, load_tls_certificate(sni_files[i], sni.identity_.private_key_password_), target_resource);
    }
    return tls;
}

void configure_http_server_tls_identity(SSL_CTX* context_value, const tls_certificate& certificate,
    const std::optional<http_server_listener_definition::tls_client_certificate_policy>&
        client_certificates) {
    if (context_value == nullptr) {
        throw std::invalid_argument("TLS context must not be null");
    }
    certificate.install(*context_value);

    if (!client_certificates.has_value()) {
        return;
    }
    client_certificates->client_ca_.install(*context_value);
    // A server that verifies peers without a session ID context turns every
    // resumption attempt into a fatal handshake error (SSL_CTX_set_session_id_context(3)).
    static constexpr unsigned char session_id_context[] = {'r', 'u', 'v', 'i', 'a'};
    ::ERR_clear_error();
    if (::SSL_CTX_set_session_id_context(
            context_value, session_id_context, sizeof(session_id_context)) != 1) {
        throw_tls_credential_error("set_session_id_context");
    }
    SSL_CTX_set_verify(context_value,
        static_cast<int>(http_server_tls_verify_mode(client_certificates->requirement_)), nullptr);
}

}  // namespace ruvia::detail
