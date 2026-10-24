#include "tls/tls_protocol_policy.h"

#include <stdexcept>

#include <openssl/ssl.h>

namespace ruvia::detail {

void apply_tls_protocol_policy(SSL_CTX& context) {
    static constexpr char tls12_cipher_suites[] =
        "ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256:"
        "ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384:"
        "ECDHE-ECDSA-CHACHA20-POLY1305:ECDHE-RSA-CHACHA20-POLY1305";
    if (SSL_CTX_set_min_proto_version(&context, TLS1_2_VERSION) != 1 ||
        SSL_CTX_set_max_proto_version(&context, 0) != 1) {
        throw std::runtime_error("failed to configure TLS versions");
    }
    SSL_CTX_set_options(&context, SSL_OP_NO_COMPRESSION | SSL_OP_NO_RENEGOTIATION);
    if (SSL_CTX_set_cipher_list(&context, tls12_cipher_suites) != 1) {
        throw std::runtime_error("failed to configure TLS 1.2 cipher suites");
    }
}

}  // namespace ruvia::detail
