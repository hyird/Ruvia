#pragma once

#include <openssl/types.h>

namespace ruvia::detail {

// The protocol policy of every client and server TLS context. Startup only.
// - TLS 1.2 or newer, no TLS compression.
// - No renegotiation: RFC 9113 §9.2.1 requires it disabled for HTTP/2 over
//   TLS 1.2, and nothing here relies on it for other protocols.
// - TLS 1.2 cipher suites limited to ECDHE key exchange with AEAD ciphers
//   (RFC 9325 §4.1-4.2; none is on RFC 9113 Appendix A's prohibited list),
//   TLS_ECDHE_*_WITH_AES_128_GCM_SHA256 first (RFC 9325 §4.2.1). TLS 1.3
//   suites, all AEAD, keep OpenSSL's defaults.
// Servers additionally choose by their own preference order.
void apply_tls_protocol_policy(SSL_CTX& context);

}  // namespace ruvia::detail
