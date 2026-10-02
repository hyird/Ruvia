#pragma once

#include <memory>
#include <memory_resource>
#include <string_view>

#include <openssl/types.h>

#include "ruvia/web/detail/client/ClientTransport.h"

namespace ruvia::detail {

// Owns the preconfigured TLS context used by outbound QUIC connections.
class http3_quic_client_tls_context final {
public:
    // The memory resource must outlive this context; host normalization is temporary.
    explicit http3_quic_client_tls_context(ClientTransportConfigView config,
        std::pmr::memory_resource* resource = std::pmr::get_default_resource());
    ~http3_quic_client_tls_context();

    http3_quic_client_tls_context(const http3_quic_client_tls_context&) = delete;
    http3_quic_client_tls_context& operator=(const http3_quic_client_tls_context&) = delete;
    http3_quic_client_tls_context(http3_quic_client_tls_context&&) = delete;
    http3_quic_client_tls_context& operator=(http3_quic_client_tls_context&&) = delete;

    [[nodiscard]] SSL_CTX* native_handle() const noexcept;

    // Configures a single connection's h3 ALPN and peer identity checks.
    // The SSL object must have been created from this context.
    void prepare(SSL* ssl, std::string_view host) const;

private:
    struct context_deleter {
        void operator()(SSL_CTX* context) const noexcept;
    };
    std::unique_ptr<SSL_CTX, context_deleter> context_;
    std::pmr::memory_resource* resource_;
};

}  // namespace ruvia::detail
