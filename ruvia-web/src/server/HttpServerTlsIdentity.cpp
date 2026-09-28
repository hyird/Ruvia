#include "ruvia/web/detail/server/tls/HttpServerTlsIdentity.h"

#include <cstring>
#include <stdexcept>

#include <asio/error.hpp>
#include <asio/ssl/verify_mode.hpp>
#include <asio/system_error.hpp>
#include <openssl/err.h>
#include <openssl/ssl.h>

#include "ruvia/web/detail/server/tls/HttpServerTlsVerify.h"

namespace ruvia::detail {
namespace {

int copyPrivateKeyPassword(char* buffer, int bufferSize, int, void* userData) noexcept {
    if (buffer == nullptr || bufferSize <= 0 || userData == nullptr) {
        return 0;
    }

    const auto& password = *static_cast<const std::pmr::string*>(userData);
    const auto capacity = static_cast<std::size_t>(bufferSize);
    if (password.size() >= capacity) {
        return 0;
    }

    std::memcpy(buffer, password.data(), password.size());
    buffer[password.size()] = '\0';
    return static_cast<int>(password.size());
}

[[nodiscard]] asio::error_code translateOpenSslError(unsigned long error) {
#if (OPENSSL_VERSION_NUMBER >= 0x30000000L)
    if (ERR_SYSTEM_ERROR(error)) {
        return asio::error_code(ERR_GET_REASON(error), asio::error::get_system_category());
    }
#endif
    return asio::error_code(static_cast<int>(error), asio::error::get_ssl_category());
}

[[noreturn]] void throwTlsIdentityError(const char* operation) {
    throw asio::system_error(translateOpenSslError(::ERR_get_error()), operation);
}

}  // namespace

void configureHttpServerTlsIdentity(SSL_CTX* context,
    const HttpServerListenerDefinition::TlsIdentity& identity,
    const std::optional<HttpServerListenerDefinition::TlsClientCertificatePolicy>&
        clientCertificates) {
    if (context == nullptr) {
        throw std::invalid_argument("TLS context must not be null");
    }
    if (identity.certificateChainFile.empty() || identity.privateKeyFile.empty()) {
        throw std::invalid_argument("TLS requires certificate chain and private key files");
    }

    if (!identity.privateKeyPassword.empty()) {
        SSL_CTX_set_default_passwd_cb(context, copyPrivateKeyPassword);
        // SSL_CTX retains this borrowed pointer. The identity storage must outlive the context.
        SSL_CTX_set_default_passwd_cb_userdata(
            context, const_cast<std::pmr::string*>(&identity.privateKeyPassword));
    }

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
    if (clientCertificates->verifyFile.empty()) {
        throw std::invalid_argument("TLS client certificate CA bundle must not be empty");
    }

    ::ERR_clear_error();
    if (::SSL_CTX_load_verify_locations(context, clientCertificates->verifyFile.c_str(), nullptr) !=
        1) {
        throwTlsIdentityError("load_verify_file");
    }
    SSL_CTX_set_verify(context,
        static_cast<int>(httpServerTlsVerifyMode(clientCertificates->requirement)), nullptr);
}

}  // namespace ruvia::detail
