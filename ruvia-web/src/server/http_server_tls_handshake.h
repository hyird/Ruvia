#pragma once

#include <memory>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

#include <asio/ip/tcp.hpp>
#include <asio/ssl.hpp>
#include <openssl/bio.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include "server/server_tls_stream.h"

// The two pieces the TLS half of a session needs: how a server-side handshake
// is initiated as an async operation, and how the verified peer identity is
// read off the finished connection.

namespace ruvia::detail {

// Surface certificate presence independently of its Subject: RFC 5280 section
// 4.1.2.6 permits an empty DN with a critical subjectAltName. Conversion failure
// must not turn a verified identity into the "no certificate" alternative.
inline std::optional<std::string_view> extract_tls_client_certificate(SSL* ssl, std::pmr::string& out) {
    out.clear();
    const X509* const certificate = SSL_get0_peer_certificate(ssl);
    if (certificate == nullptr) {
        return std::nullopt;
    }
    const X509_NAME* subject = X509_get_subject_name(certificate);
    if (subject == nullptr) {
        throw std::runtime_error("TLS client certificate has no subject");
    }
    // Render the DN in RFC 2253 form through a memory BIO. Unlike
    // X509_NAME_oneline into a fixed 256-byte buffer, this captures the full
    // subject without silent truncation and produces the unambiguous,
    // standard-parseable form recommended for authorization.
    const auto bio = std::unique_ptr<BIO, decltype(&BIO_free)>(BIO_new(BIO_s_mem()), &BIO_free);
    if (bio == nullptr) {
        throw std::runtime_error("TLS client certificate subject buffer could not be allocated");
    }
    if (X509_NAME_print_ex(bio.get(), subject, 0, XN_FLAG_RFC2253) < 0) {
        throw std::runtime_error("TLS client certificate subject could not be rendered");
    }
    char* data = nullptr;
    const long length = BIO_get_mem_data(bio.get(), &data);
    if (length < 0 || (length > 0 && data == nullptr)) {
        throw std::runtime_error("TLS client certificate subject could not be read");
    }
    if (length > 0) {
        out.assign(data, static_cast<std::size_t>(length));
    }
    return std::string_view(out);
}

struct tls_server_handshake_initiator final {
    server_tls_stream* stream_;

    template <typename handler_type>
    void operator()(handler_type handler) const {
        stream_->async_handshake(asio::ssl::stream_base::server, std::move(handler));
    }
};

}  // namespace ruvia::detail
