#include "server/http_server_tls_identity.h"

#include <stdexcept>

#include <asio/error.hpp>
#include <asio/ssl/verify_mode.hpp>
#include <asio/system_error.hpp>
#include <openssl/err.h>
#include <openssl/ssl.h>

#include "server/http_server_tls_verify.h"
#include "tls/tls_file_paths.h"
#include "tls/tls_password_scope.h"

namespace ruvia::detail {
namespace {

[[nodiscard]] asio::error_code translate_open_ssl_error(unsigned long error) {
    if (ERR_SYSTEM_ERROR(error)) {
        return asio::error_code(ERR_GET_REASON(error), asio::error::get_system_category());
    }
    return asio::error_code(static_cast<int>(error), asio::error::get_ssl_category());
}

[[noreturn]] void throw_tls_identity_error(const char* operation) {
    throw asio::system_error(translate_open_ssl_error(::ERR_get_error()), operation);
}

}  // namespace

void validate_http_server_tls_identity(
    const http_server_listener_definition::tls_identity_type& identity) {
    if (identity.certificate_chain_file_.empty() || identity.private_key_file_.empty()) {
        throw std::invalid_argument(
            "TLS certificate chain and private key files must not be empty");
    }
    validate_tls_file_paths({identity.certificate_chain_file_, identity.private_key_file_});
}

void validate_http_server_tls_client_certificate_policy(
    const http_server_listener_definition::tls_client_certificate_policy_type& policy) {
    switch (policy.requirement_) {
        case tls_client_certificate_requirement::optional:
        case tls_client_certificate_requirement::required:
            break;
        default:
            throw std::invalid_argument("TLS client certificate requirement is invalid");
    }
    if (policy.verify_file_.empty()) {
        throw std::invalid_argument("TLS client certificate CA bundle must not be empty");
    }
    validate_tls_file_paths({policy.verify_file_});
}

void configure_http_server_tls_identity(SSL_CTX* context_value,
    const http_server_listener_definition::tls_identity_type& identity,
    const std::optional<http_server_listener_definition::tls_client_certificate_policy_type>&
        client_certificates) {
    if (context_value == nullptr) {
        throw std::invalid_argument("TLS context must not be null");
    }
    validate_http_server_tls_identity(identity);
    if (client_certificates) {
        validate_http_server_tls_client_certificate_policy(*client_certificates);
    }

    const tls_password_scope password_scope(*context_value, identity.private_key_password_);

    ::ERR_clear_error();
    if (::SSL_CTX_use_certificate_chain_file(context_value, identity.certificate_chain_file_.c_str()) != 1) {
        throw_tls_identity_error("use_certificate_chain_file");
    }

    ::ERR_clear_error();
    if (::SSL_CTX_use_PrivateKey_file(
            context_value, identity.private_key_file_.c_str(), SSL_FILETYPE_PEM) != 1) {
        throw_tls_identity_error("use_private_key_file");
    }

    ::ERR_clear_error();
    if (::SSL_CTX_check_private_key(context_value) != 1) {
        throw_tls_identity_error("check_private_key");
    }

    if (!client_certificates.has_value()) {
        return;
    }
    ::ERR_clear_error();
    if (::SSL_CTX_load_verify_file(context_value, client_certificates->verify_file_.c_str()) !=
        1) {
        throw_tls_identity_error("load_verify_file");
    }
    // A server that verifies peers without a session ID context turns every
    // resumption attempt into a fatal handshake error (SSL_CTX_set_session_id_context(3)).
    static constexpr unsigned char session_id_context[] = {'r', 'u', 'v', 'i', 'a'};
    ::ERR_clear_error();
    if (::SSL_CTX_set_session_id_context(
            context_value, session_id_context, sizeof(session_id_context)) != 1) {
        throw_tls_identity_error("set_session_id_context");
    }
    SSL_CTX_set_verify(context_value,
        static_cast<int>(http_server_tls_verify_mode(client_certificates->requirement_)), nullptr);
}

}  // namespace ruvia::detail
