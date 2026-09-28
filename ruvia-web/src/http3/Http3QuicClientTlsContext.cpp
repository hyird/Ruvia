#include "ruvia/web/detail/http3/Http3QuicClientTlsContext.h"

#include <array>
#include <stdexcept>
#include <string>

#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include "ruvia/core/DnsHost.h"

namespace ruvia::detail {
namespace {

int readPrivateKeyPassword(char* buffer, int size, int, void* argument) noexcept {
    if (buffer == nullptr || size <= 0 || argument == nullptr) {
        return 0;
    }
    const auto password = static_cast<const ClientTransportConfigView*>(argument)->privateKeyPassword;
    if (password.size() > static_cast<std::size_t>(size)) {
        return 0;
    }
    for (std::size_t index = 0; index < password.size(); ++index) {
        buffer[index] = password[index];
    }
    return static_cast<int>(password.size());
}

}  // namespace

Http3QuicClientTlsContext::Http3QuicClientTlsContext(ClientTransportConfigView config) {
    validateClientTransportConfig(config);
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    (void)config;
    throw std::runtime_error("QUIC client TLS requires OpenSSL 3.6 or newer");
#else
    context_.reset(SSL_CTX_new(OSSL_QUIC_client_method()));
    if (!context_) {
        throw std::runtime_error("failed to create QUIC client TLS context");
    }

    SSL_CTX_set_verify(context_.get(),
        config.tlsPeerVerification == TlsPeerVerificationPolicy::kVerify
            ? SSL_VERIFY_PEER
            : SSL_VERIFY_NONE,
        nullptr);
    if (config.tlsPeerVerification == TlsPeerVerificationPolicy::kVerify) {
        const int loaded = config.caFile.empty()
                               ? SSL_CTX_set_default_verify_paths(context_.get())
                               : SSL_CTX_load_verify_file(context_.get(),
                                     std::string(config.caFile).c_str());
        if (loaded != 1) {
            throw std::runtime_error("failed to load client TLS trust store");
        }
    }

    if (!config.certificateChainFile.empty()) {
        if (SSL_CTX_use_certificate_chain_file(context_.get(),
                std::string(config.certificateChainFile).c_str()) != 1) {
            throw std::runtime_error("failed to load client certificate chain");
        }
        if (!config.privateKeyPassword.empty()) {
            SSL_CTX_set_default_passwd_cb(context_.get(), &readPrivateKeyPassword);
            SSL_CTX_set_default_passwd_cb_userdata(context_.get(), &config);
        }
        const int keyLoaded = SSL_CTX_use_PrivateKey_file(context_.get(),
            std::string(config.privateKeyFile).c_str(), SSL_FILETYPE_PEM);
        // Never retain either the callback or its borrowed configuration view.
        SSL_CTX_set_default_passwd_cb(context_.get(), nullptr);
        SSL_CTX_set_default_passwd_cb_userdata(context_.get(), nullptr);
        if (keyLoaded != 1 || SSL_CTX_check_private_key(context_.get()) != 1) {
            throw std::runtime_error("failed to load or match client TLS private key");
        }
    }
#endif
}

Http3QuicClientTlsContext::~Http3QuicClientTlsContext() = default;

SSL_CTX* Http3QuicClientTlsContext::nativeHandle() const noexcept {
    return context_.get();
}

void Http3QuicClientTlsContext::prepare(SSL* ssl, std::string_view host) const {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    (void)ssl;
    (void)host;
    throw std::runtime_error("QUIC client TLS requires OpenSSL 3.6 or newer");
#else
    if (ssl == nullptr || !SSL_is_quic(ssl) || SSL_get_SSL_CTX(ssl) != context_.get() ||
        SSL_get_SSL_CTX(ssl) == nullptr ||
        SSL_CTX_get_ssl_method(SSL_get_SSL_CTX(ssl)) != OSSL_QUIC_client_method()) {
        throw std::invalid_argument("SSL connection does not use this QUIC client TLS context");
    }
    validateClientOriginHost(host, "client TLS host is empty", "client TLS host is invalid");

    const bool ipAddress = isClientIpAddress(host);
    std::string normalized(host);
    if (!ipAddress && normalized.ends_with('.')) {
        normalized.pop_back();
    }
    if (!ipAddress && SSL_set_tlsext_host_name(ssl, normalized.c_str()) != 1) {
        throw std::runtime_error("failed to set client TLS SNI host");
    }

    if (SSL_set_alpn_protos(ssl, std::array<unsigned char, 3>{2, 'h', '3'}.data(), 3) != 0) {
        throw std::runtime_error("failed to configure h3 ALPN");
    }

    if (SSL_CTX_get_verify_mode(context_.get()) == SSL_VERIFY_PEER) {
        X509_VERIFY_PARAM* const parameters = SSL_get0_param(ssl);
        const int configured = ipAddress
                                   ? X509_VERIFY_PARAM_set1_ip_asc(parameters, normalized.c_str())
                                   : SSL_set1_host(ssl, normalized.c_str());
        if (configured != 1) {
            throw std::runtime_error("failed to configure client TLS peer host verification");
        }
    }
#endif
}

void Http3QuicClientTlsContext::ContextDeleter::operator()(SSL_CTX* context) const noexcept {
    SSL_CTX_free(context);
}

}  // namespace ruvia::detail
