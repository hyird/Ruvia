#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include <openssl/bio.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

// What a finished TLS handshake negotiated with the peer: the selected ALPN
// protocol, the offered server_name and the verified peer certificate identity.

namespace ruvia::detail {

// The ALPN protocol the handshake selected; empty when none was negotiated.
[[nodiscard]] inline std::string_view tls_selected_alpn(SSL& ssl) noexcept {
    const unsigned char* selected = nullptr;
    unsigned int size = 0;
    SSL_get0_alpn_selected(&ssl, &selected, &size);
    return {reinterpret_cast<const char*>(selected), size};
}

// The host_name the client offered in server_name (RFC 6066 §3); empty when
// it offered none. The view lives as long as the SSL.
[[nodiscard]] inline std::string_view tls_server_name(SSL& ssl) noexcept {
    const char* name = SSL_get_servername(&ssl, TLSEXT_NAMETYPE_host_name);
    return name == nullptr ? std::string_view{} : std::string_view(name);
}

// Surface certificate presence independently of its Subject: RFC 5280 section
// 4.1.2.6 permits an empty DN with a critical subjectAltName. Conversion failure
// must not turn a verified identity into the "no certificate" alternative.
inline std::optional<std::string_view> extract_tls_client_certificate(SSL* ssl, std::string& out) {
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

}  // namespace ruvia::detail
