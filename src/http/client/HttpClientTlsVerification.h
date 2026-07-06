#pragma once

#ifdef RUVIA_ENABLE_HTTP_CLIENT

#include <asio/error_code.hpp>
#include <asio/ip/address.hpp>
#include <asio/ssl/context.hpp>
#include <asio/ssl/verify_context.hpp>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#include "ruvia/memory/PmrResource.h"

namespace ruvia::detail {

// Build the shared client TLS context from the config's Tls options: trust the system store (or a
// custom CA bundle), verify the peer chain (unless insecureSkipVerify), and present a client
// certificate for mutual TLS if configured. The per-connection host-name match is layered on
// separately via applyClientTlsIdentity. Sole owner shared by the HTTP/1.1 pool and HTTP/2 session.
inline void configureClientTlsContext(
    std::optional<asio::ssl::context>& context, const HttpClientConfig::Tls& tls) {
    context.emplace(asio::ssl::context::tls_client);
    if (tls.insecureSkipVerify) {
        context->set_verify_mode(asio::ssl::verify_none);
    } else {
        if (tls.caFile.empty()) {
            context->set_default_verify_paths();
        } else {
            context->load_verify_file(std::string(tls.caFile));
        }
        context->set_verify_mode(asio::ssl::verify_peer);
    }
    if (!tls.certificateChainFile.empty() || !tls.privateKeyFile.empty()) {
        if (tls.certificateChainFile.empty() || tls.privateKeyFile.empty()) {
            throw std::invalid_argument(
                "http client mTLS requires both certificateChainFile and privateKeyFile");
        }
        if (!tls.privateKeyPassword.empty()) {
            const std::string password(tls.privateKeyPassword);
            context->set_password_callback(
                [password](std::size_t, asio::ssl::context::password_purpose) { return password; });
        }
        context->use_certificate_chain_file(std::string(tls.certificateChainFile));
        context->use_private_key_file(std::string(tls.privateKeyFile), asio::ssl::context::pem);
    }
}

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
// `verifyHost` is the name advertised via SNI and matched against the certificate (config.host, or
// tlsOptions.sniHost when set to decouple it from the connect address). When insecureSkipVerify is
// set the context already uses verify_none, so no host-name callback is installed.
template <typename TlsStream>
inline void applyClientTlsIdentity(
    TlsStream& tlsStream,
    const std::pmr::string& verifyHost,
    bool insecureSkipVerify,
    std::pmr::memory_resource* resource) {
    std::error_code addressEc;
    asio::ip::make_address(std::string_view(verifyHost), addressEc);
    if (addressEc) {  // not an IP literal -> advertise SNI
        // OpenSSL needs a NUL-terminated string; the pmr::string is one.
        if (SSL_set_tlsext_host_name(tlsStream.native_handle(), verifyHost.c_str()) != 1) {
            throw std::runtime_error("http client: failed to set TLS SNI host name");
        }
    }
    if (!insecureSkipVerify) {
        tlsStream.set_verify_callback(
            HttpClientHostNameVerification(std::string_view(verifyHost), resource));
    }
}

}  // namespace ruvia::detail

#endif  // RUVIA_ENABLE_HTTP_CLIENT
