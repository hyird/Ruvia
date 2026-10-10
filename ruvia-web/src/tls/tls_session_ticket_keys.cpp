#include "tls/tls_session_ticket_keys.h"

#include <stdexcept>

#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>

namespace ruvia::detail {

tls_session_ticket_keys tls_session_ticket_keys::generate() {
    tls_session_ticket_keys keys;
    if (RAND_bytes(keys.bytes_.data(), static_cast<int>(keys.bytes_.size())) != 1) {
        throw std::runtime_error("failed to generate TLS session ticket keys");
    }
    return keys;
}

tls_session_ticket_keys::~tls_session_ticket_keys() {
    OPENSSL_cleanse(bytes_.data(), bytes_.size());
}

void tls_session_ticket_keys::install(SSL_CTX& context) const {
    // OpenSSL copies the keys into the context; the control API is not
    // const-correct but does not modify its input.
    if (SSL_CTX_set_tlsext_ticket_keys(&context, const_cast<unsigned char*>(bytes_.data()),
            static_cast<long>(bytes_.size())) != 1) {
        throw std::runtime_error("failed to install TLS session ticket keys");
    }
}

}  // namespace ruvia::detail
