#pragma once

#include <asio/ssl/verify_mode.hpp>

namespace ruvia::detail {

// The peer-verification mode applied once a client-CA verifyFile is configured.
// verify_peer validates a client certificate that is presented but still admits
// a client that presents none (optional mutual TLS); adding
// verify_fail_if_no_peer_cert makes a missing certificate fail the handshake
// (mandatory mutual TLS).
[[nodiscard]] inline asio::ssl::verify_mode httpServerTlsVerifyMode(
    bool requireClientCertificate) noexcept {
    auto mode = asio::ssl::verify_peer;
    if (requireClientCertificate) {
        mode |= asio::ssl::verify_fail_if_no_peer_cert;
    }
    return mode;
}

}  // namespace ruvia::detail
