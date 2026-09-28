#include "ruvia/web/detail/http3/Http3QuicTlsContext.h"

#include <stdexcept>
#include <utility>

#include <openssl/ssl.h>

#include "ruvia/http/HttpAscii.h"
#include "ruvia/web/detail/server/tls/HttpServerTlsIdentity.h"

namespace ruvia::detail {
namespace {

void configureQuicTlsContext(SSL_CTX* context,
    const HttpServerListenerDefinition::TlsIdentity& identity,
    const std::optional<HttpServerListenerDefinition::TlsClientCertificatePolicy>&
        clientCertificates,
    int (*alpnCallback)(SSL*, const unsigned char**, unsigned char*, const unsigned char*,
        unsigned int, void*) noexcept) {
    if (context == nullptr) {
        throw std::runtime_error("failed to create QUIC TLS context");
    }
    // OpenSSL's QUIC method uses TLS 1.3 exclusively. Its min/max protocol
    // setters can report success while the corresponding getters remain zero.
    SSL_CTX_set_options(context, SSL_OP_NO_COMPRESSION);
    SSL_CTX_set_alpn_select_cb(context, alpnCallback, nullptr);
    configureHttpServerTlsIdentity(context, identity, clientCertificates);
    // Password callback data belongs to the input configuration; credentials are now loaded.
    SSL_CTX_set_default_passwd_cb(context, nullptr);
    SSL_CTX_set_default_passwd_cb_userdata(context, nullptr);
}

void configureIdentityContext(SSL_CTX* context,
    const HttpServerListenerDefinition::TlsIdentity& identity) {
    if (context == nullptr) {
        throw std::runtime_error("failed to create QUIC SNI identity context");
    }
    configureHttpServerTlsIdentity(context, identity, std::nullopt);
    // Do not retain the configuration's password storage after preloading the key.
    SSL_CTX_set_default_passwd_cb(context, nullptr);
    SSL_CTX_set_default_passwd_cb_userdata(context, nullptr);
}

}  // namespace

Http3QuicTlsContext::Http3QuicTlsContext(const HttpServerListenerDefinition::Tls& tls,
    std::pmr::memory_resource* resource)
    : identityContexts_(pmrResourceOrDefault(resource)),
      sniIdentities_(pmrResourceOrDefault(resource)) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    (void)tls;
    throw std::runtime_error("QUIC server TLS requires OpenSSL 3.6 or newer");
#else
    identityContexts_.reserve(tls.sniIdentities.size());
    sniIdentities_.reserve(tls.sniIdentities.size());

    for (const auto& configured : tls.sniIdentities) {
        ContextOwner identityContext(SSL_CTX_new(OSSL_QUIC_server_method()));
        configureIdentityContext(identityContext.get(), configured.identity);
        SSL_CTX* const rawContext = identityContext.get();
        X509* const certificate = SSL_CTX_get0_certificate(rawContext);
        EVP_PKEY* const privateKey = SSL_CTX_get0_privatekey(rawContext);
        STACK_OF(X509)* chain = nullptr;
        if (certificate == nullptr || privateKey == nullptr ||
            SSL_CTX_get0_chain_certs(rawContext, &chain) != 1) {
            throw std::runtime_error("failed to preload QUIC SNI certificate identity");
        }
        identityContexts_.push_back(std::move(identityContext));
        sniIdentities_.push_back(
            SniIdentity{std::pmr::string(configured.host, sniIdentities_.get_allocator().resource()),
                rawContext, certificate, privateKey, chain});
    }

    ContextOwner defaultContext(SSL_CTX_new(OSSL_QUIC_server_method()));
    configureQuicTlsContext(defaultContext.get(), tls.identity, tls.clientCertificates,
        &selectAlpnProtocol);
    defaultContext_ = defaultContext.get();
    SSL_CTX_set_cert_cb(defaultContext_, &selectCertificate, this);
    // SSL objects and active QUIC connections borrow this context's cert callback
    // argument, and SNI cert_cb copies from identityContexts_; all must stay alive.
    defaultContextOwner_ = std::move(defaultContext);
#endif
}

Http3QuicTlsContext::~Http3QuicTlsContext() = default;

void Http3QuicTlsContext::ContextDeleter::operator()(SSL_CTX* context) const noexcept {
    SSL_CTX_free(context);
}

int Http3QuicTlsContext::selectCertificate(SSL* ssl, void* argument) noexcept {
    if (ssl == nullptr || argument == nullptr) {
        return 0;
    }
    const char* const serverName = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
    if (serverName == nullptr) {
        return 1;
    }

    const auto& owner = *static_cast<const Http3QuicTlsContext*>(argument);
    for (const auto& entry : owner.sniIdentities_) {
        if (!httpAsciiEqualsIgnoreCase(entry.host, serverName)) {
            continue;
        }

        // The certificate, key, and chain are borrowed from entry.context, which
        // remains owned for the lifetime of this callback. OpenSSL retains what
        // it needs on the SSL object; the helper context continues owning the chain.
        return SSL_use_cert_and_key(
                   ssl, entry.certificate, entry.privateKey, entry.chain, 1) == 1
                   ? 1
                   : 0;
    }
    return 1;
}

int Http3QuicTlsContext::selectAlpnProtocol(SSL*, const unsigned char** output,
    unsigned char* outputLength, const unsigned char* input, unsigned int inputLength,
    void*) noexcept {
    constexpr unsigned char h3[] = {'h', '3'};
    for (unsigned int offset = 0; offset < inputLength;) {
        const unsigned int length = input[offset++];
        if (length > inputLength - offset) {
            return SSL_TLSEXT_ERR_ALERT_FATAL;
        }
        if (length == sizeof(h3) && input[offset] == h3[0] && input[offset + 1] == h3[1]) {
            *output = input + offset;
            *outputLength = static_cast<unsigned char>(sizeof(h3));
            return SSL_TLSEXT_ERR_OK;
        }
        offset += length;
    }
    return SSL_TLSEXT_ERR_ALERT_FATAL;
}

}  // namespace ruvia::detail
