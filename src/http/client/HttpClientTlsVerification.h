#pragma once

#ifdef RUVIA_ENABLE_HTTP_CLIENT

#include <asio/error_code.hpp>
#include <asio/ip/address.hpp>
#include <asio/ssl/verify_context.hpp>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#include "ruvia/memory/PmrResource.h"

namespace ruvia::detail {

// Asio's host_name_verification owns a std::string. Keep the same OpenSSL/RFC 6125
// behavior while binding the hostname storage to the connection/session PMR resource.
class HttpClientHostNameVerification final {
public:
    explicit HttpClientHostNameVerification(
        std::string_view host,
        std::pmr::memory_resource* resource = nullptr)
        : resource_(pmrResourceOrDefault(resource)),
          host_(host.data(), host.size(), resource_) {}

    HttpClientHostNameVerification(const HttpClientHostNameVerification& other)
        : resource_(other.resource_),
          host_(other.host_, resource_) {}

    HttpClientHostNameVerification& operator=(const HttpClientHostNameVerification&) = delete;
    HttpClientHostNameVerification(HttpClientHostNameVerification&&) noexcept = default;
    HttpClientHostNameVerification& operator=(HttpClientHostNameVerification&&) noexcept = default;

    bool operator()(bool preverified, asio::ssl::verify_context& ctx) const {
        if (!preverified) {
            return false;
        }

        if (X509_STORE_CTX_get_error_depth(ctx.native_handle()) > 0) {
            return true;
        }

        asio::error_code ec;
        (void)asio::ip::make_address(std::string_view(host_.data(), host_.size()), ec);
        X509* cert = X509_STORE_CTX_get_current_cert(ctx.native_handle());
        if (!ec) {
            return X509_check_ip_asc(cert, host_.c_str(), 0) == 1;
        }

        char* peerName = nullptr;
        const int result = X509_check_host(cert, host_.c_str(), host_.size(), 0, &peerName);
        OPENSSL_free(peerName);
        return result == 1;
    }

private:
    std::pmr::memory_resource* resource_;
    std::pmr::string host_;
};

// Apply the per-connection client TLS identity checks to `tlsStream` for `host`:
// RFC 6066 SNI (advertised for host names only, never IP literals, since SNI must
// carry a name not an address) plus the RFC 6125 host-name verification callback.
// Sole owner shared by the HTTP/1.1 pool and the HTTP/2 session so the two cannot
// silently diverge on a security-critical step (e.g. one omitting the callback,
// which would leave verify_peer checking only the chain and not the host).
template <typename TlsStream>
inline void applyClientTlsIdentity(
    TlsStream& tlsStream,
    const std::pmr::string& host,
    std::pmr::memory_resource* resource) {
    std::error_code addressEc;
    asio::ip::make_address(std::string_view(host), addressEc);
    if (addressEc) {  // not an IP literal -> advertise SNI
        // OpenSSL needs a NUL-terminated string; the pmr::string is one.
        if (SSL_set_tlsext_host_name(tlsStream.native_handle(), host.c_str()) != 1) {
            throw std::runtime_error("http client: failed to set TLS SNI host name");
        }
    }
    tlsStream.set_verify_callback(HttpClientHostNameVerification(std::string_view(host), resource));
}

}  // namespace ruvia::detail

#endif  // RUVIA_ENABLE_HTTP_CLIENT
