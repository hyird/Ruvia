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
class http3_quic_tls_context final {
public:
    http3_quic_tls_context(const HttpServerListenerDefinition::Tls& tls,
        std::pmr::memory_resource* resource);
    ~http3_quic_tls_context();

    http3_quic_tls_context(const http3_quic_tls_context&) = delete;
    http3_quic_tls_context& operator=(const http3_quic_tls_context&) = delete;
    http3_quic_tls_context(http3_quic_tls_context&&) = delete;
    http3_quic_tls_context& operator=(http3_quic_tls_context&&) = delete;

    [[nodiscard]] SSL_CTX* default_context() const noexcept {
        return default_context_;
    }
    [[nodiscard]] bool early_data_enabled() const noexcept {
        return early_data_enabled_;
    }

private:
    struct sni_identity final {
        std::pmr::string host;
        SSL_CTX* context;
        X509* certificate;
        EVP_PKEY* private_key;
        STACK_OF(X509) * chain;
    };

    static int select_certificate(SSL* ssl, void* argument) noexcept;
    static int select_alpn_protocol(SSL* ssl, const unsigned char** output,
        unsigned char* output_length, const unsigned char* input, unsigned int input_length,
        void* argument) noexcept;

    struct context_deleter {
        void operator()(SSL_CTX* context) const noexcept;
    };
    using context_owner = std::unique_ptr<SSL_CTX, context_deleter>;

    // Auxiliary contexts own the cert/key/chain that the callback borrows.
    context_owner default_context_owner_;
    std::pmr::vector<context_owner> identity_contexts_;
    std::pmr::vector<sni_identity> sni_identities_;
    SSL_CTX* default_context_{nullptr};
    bool early_data_enabled_{};
};

}  // namespace ruvia::detail
