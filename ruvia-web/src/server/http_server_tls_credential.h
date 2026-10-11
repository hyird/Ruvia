#pragma once

#include <openssl/x509.h>

namespace ruvia::detail {

// A server certificate, its chain and private key, parsed from PEM once when
// the server configuration is validated and installed into every worker's
// TLS SSL_CTX. Copies share the immutable OpenSSL objects through
// their own reference counts, so workers never re-read or re-parse the files
// (and cannot observe different files if they rotate during startup).
class http_server_tls_certificate final {
public:
    http_server_tls_certificate(X509* certificate, STACK_OF(X509) * chain, EVP_PKEY* private_key);
    http_server_tls_certificate(const http_server_tls_certificate& other);
    http_server_tls_certificate& operator=(const http_server_tls_certificate& other);
    ~http_server_tls_certificate();

    // Startup only: the context is not yet shared with any connection.
    void install(SSL_CTX& context) const;

private:
    void release() noexcept;

    X509* certificate_{};
    STACK_OF(X509) * chain_ {};
    EVP_PKEY* private_key_{};
};

// The client-certificate trust store loaded once from the configured CA
// bundle, shared read-only by every worker's verifying context.
class http_server_tls_client_ca final {
public:
    explicit http_server_tls_client_ca(X509_STORE* store);
    http_server_tls_client_ca(const http_server_tls_client_ca& other);
    http_server_tls_client_ca& operator=(const http_server_tls_client_ca& other);
    ~http_server_tls_client_ca();

    [[nodiscard]] X509_STORE* store() const noexcept {
        return store_;
    }

private:
    X509_STORE* store_{};
};

}  // namespace ruvia::detail
