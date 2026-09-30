#pragma once

#include <memory>
#include <string_view>

#include <openssl/types.h>

#include "ruvia/web/detail/client/ClientTransport.h"

namespace ruvia::detail {

// Owns the preconfigured TLS context used by outbound QUIC connections.
class Http3QuicClientTlsContext final {
public:
    explicit Http3QuicClientTlsContext(ClientTransportConfigView config);
    ~Http3QuicClientTlsContext();

    Http3QuicClientTlsContext(const Http3QuicClientTlsContext&) = delete;
    Http3QuicClientTlsContext& operator=(const Http3QuicClientTlsContext&) = delete;
    Http3QuicClientTlsContext(Http3QuicClientTlsContext&&) = delete;
    Http3QuicClientTlsContext& operator=(Http3QuicClientTlsContext&&) = delete;

    [[nodiscard]] SSL_CTX* nativeHandle() const noexcept;

    // Configures a single connection's h3 ALPN and peer identity checks.
    // The SSL object must have been created from this context.
    void prepare(SSL* ssl, std::string_view host) const;

private:
    struct ContextDeleter {
        void operator()(SSL_CTX* context) const noexcept;
    };
    std::unique_ptr<SSL_CTX, ContextDeleter> context_;
};

}  // namespace ruvia::detail
