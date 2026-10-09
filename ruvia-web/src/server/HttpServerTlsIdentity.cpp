#include "server/HttpServerTlsIdentity.h"

#include <stdexcept>

#include <asio/error.hpp>
#include <asio/ssl/verify_mode.hpp>
#include <asio/system_error.hpp>
#include <openssl/err.h>
#include <openssl/ssl.h>

#include "server/HttpServerTlsVerify.h"
#include "tls/TlsFilePaths.h"
#include "tls/TlsPasswordScope.h"

namespace ruvia::detail {
namespace {

[[nodiscard]] asio::error_code translateOpenSslError(unsigned long error) {
    if (ERR_SYSTEM_ERROR(error)) {
        return asio::error_code(ERR_GET_REASON(error), asio::error::get_system_category());
    }
    return asio::error_code(static_cast<int>(error), asio::error::get_ssl_category());
}

[[noreturn]] void throwTlsIdentityError(const char* operation) {
    throw asio::system_error(translateOpenSslError(::ERR_get_error()), operation);
}

}  // namespace

void validateHttpServerTlsIdentity(
    const HttpServerListenerDefinition::TlsIdentity& identity) {
    if (identity.certificateChainFile.empty() || identity.privateKeyFile.empty()) {
        throw std::invalid_argument(
            "TLS certificate chain and private key files must not be empty");
    }
    validate_tls_file_paths({identity.certificateChainFile, identity.privateKeyFile});
}

void validateHttpServerTlsClientCertificatePolicy(
    const HttpServerListenerDefinition::TlsClientCertificatePolicy& policy) {
    switch (policy.requirement) {
        case TlsClientCertificateRequirement::kOptional:
        case TlsClientCertificateRequirement::kRequired:
            break;
        default:
            throw std::invalid_argument("TLS client certificate requirement is invalid");
    }
    if (policy.verifyFile.empty()) {
        throw std::invalid_argument("TLS client certificate CA bundle must not be empty");
    }
    validate_tls_file_paths({policy.verifyFile});
}

void configureHttpServerTlsIdentity(SSL_CTX* context,
    const HttpServerListenerDefinition::TlsIdentity& identity,
    const std::optional<HttpServerListenerDefinition::TlsClientCertificatePolicy>&
        clientCertificates) {
    if (context == nullptr) {
        throw std::invalid_argument("TLS context must not be null");
    }
    validateHttpServerTlsIdentity(identity);
    if (clientCertificates) {
        validateHttpServerTlsClientCertificatePolicy(*clientCertificates);
    }

    const tls_password_scope password_scope(*context, identity.privateKeyPassword);

    ::ERR_clear_error();
    if (::SSL_CTX_use_certificate_chain_file(context, identity.certificateChainFile.c_str()) != 1) {
        throwTlsIdentityError("use_certificate_chain_file");
    }

    ::ERR_clear_error();
    if (::SSL_CTX_use_PrivateKey_file(
            context, identity.privateKeyFile.c_str(), SSL_FILETYPE_PEM) != 1) {
        throwTlsIdentityError("use_private_key_file");
    }

    ::ERR_clear_error();
    if (::SSL_CTX_check_private_key(context) != 1) {
        throwTlsIdentityError("check_private_key");
    }

    if (!clientCertificates.has_value()) {
        return;
    }
    ::ERR_clear_error();
    if (::SSL_CTX_load_verify_file(context, clientCertificates->verifyFile.c_str()) !=
        1) {
        throwTlsIdentityError("load_verify_file");
    }
    SSL_CTX_set_verify(context,
        static_cast<int>(httpServerTlsVerifyMode(clientCertificates->requirement)), nullptr);
}

}  // namespace ruvia::detail
