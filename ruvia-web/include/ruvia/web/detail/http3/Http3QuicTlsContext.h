#pragma once

#include <memory>
#include <memory_resource>
#include <string>
#include <vector>

#include <openssl/evp.h>
#include <openssl/types.h>
#include <openssl/x509.h>

#include "ruvia/web/detail/server/HttpServerListener.h"

namespace ruvia::detail {

// Owns the default QUIC context and auxiliary contexts holding preloaded SNI identities.
// Connections created from the default context must not outlive this object.
class Http3QuicTlsContext final {
public:
    Http3QuicTlsContext(const HttpServerListenerDefinition::Tls& tls,
        std::pmr::memory_resource* resource);
    ~Http3QuicTlsContext();

    Http3QuicTlsContext(const Http3QuicTlsContext&) = delete;
    Http3QuicTlsContext& operator=(const Http3QuicTlsContext&) = delete;
    Http3QuicTlsContext(Http3QuicTlsContext&&) = delete;
    Http3QuicTlsContext& operator=(Http3QuicTlsContext&&) = delete;

    [[nodiscard]] SSL_CTX* defaultContext() const noexcept {
        return defaultContext_;
    }

private:
    struct SniIdentity final {
        std::pmr::string host;
        SSL_CTX* context;
        X509* certificate;
        EVP_PKEY* privateKey;
        STACK_OF(X509) * chain;
    };

    static int selectCertificate(SSL* ssl, void* argument) noexcept;
    static int selectAlpnProtocol(SSL* ssl, const unsigned char** output,
        unsigned char* outputLength, const unsigned char* input, unsigned int inputLength,
        void* argument) noexcept;

    struct ContextDeleter final {
        void operator()(SSL_CTX* context) const noexcept;
    };
    using ContextOwner = std::unique_ptr<SSL_CTX, ContextDeleter>;

    // Auxiliary contexts own the cert/key/chain that the callback borrows.
    ContextOwner defaultContextOwner_;
    std::pmr::vector<ContextOwner> identityContexts_;
    std::pmr::vector<SniIdentity> sniIdentities_;
    SSL_CTX* defaultContext_{nullptr};
};

}  // namespace ruvia::detail
