#pragma once

#include <memory>
#include <memory_resource>
#include <utility>

#include <asio/ip/tcp.hpp>
#include <asio/ssl.hpp>
#include <openssl/bio.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include "ruvia/web/detail/server/WebWorkerRuntime.h"

// The two pieces the TLS half of a session needs: how a server-side handshake
// is initiated as an async operation, and how the verified peer identity is
// read off the finished connection.

namespace ruvia::detail {

// Extracts the verified peer (client) certificate subject DN into `out`, or
// leaves it empty when no client certificate was presented. Used to surface
// mutual-TLS identity to handlers via getConnInfo(context).
inline void extractTlsClientCertificate(SSL* ssl, std::pmr::string& out) {
    out.clear();
    const auto certificate = std::unique_ptr<X509, decltype(&X509_free)>(SSL_get_peer_certificate(ssl), &X509_free);
    if (certificate == nullptr) {
        return;
    }
    X509_NAME* subject = X509_get_subject_name(certificate.get());
    if (subject == nullptr) {
        return;
    }
    // Render the DN in RFC 2253 form through a memory BIO. Unlike
    // X509_NAME_oneline into a fixed 256-byte buffer, this captures the full
    // subject without silent truncation and produces the unambiguous,
    // standard-parseable form recommended for authorization.
    const auto bio = std::unique_ptr<BIO, decltype(&BIO_free)>(BIO_new(BIO_s_mem()), &BIO_free);
    if (bio == nullptr) {
        return;
    }
    if (X509_NAME_print_ex(bio.get(), subject, 0, XN_FLAG_RFC2253) < 0) {
        return;
    }
    char* data = nullptr;
    const long length = BIO_get_mem_data(bio.get(), &data);
    if (data != nullptr && length > 0) {
        out.assign(data, static_cast<std::size_t>(length));
    }
}

struct TlsServerHandshakeInitiator final {
    asio::ssl::stream<TcpSocket&>* stream;

    template <typename Handler>
    void operator()(Handler handler) const {
        stream->async_handshake(asio::ssl::stream_base::server, std::move(handler));
    }
};

}  // namespace ruvia::detail
