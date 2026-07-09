#pragma once

// Outbound HTTP client PUBLIC SURFACE (configuration, fetch options, responses).
//
// OWNERSHIP: the outbound HTTP client is a ruvia::http capability: pure
// configuration, policy, parsing, redirects, decoding, and validation. The protocol
// half and the shared Http2Connection client role are sans-I/O and asio-free. Higher
// layers provide the socket/TLS runtime driver and expose product-specific APIs over
// these types.

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/http/HttpCommon.h"
#include "ruvia/http/HttpLimits.h"
#include "ruvia/http/detail/PmrResource.h"

namespace ruvia::detail {
inline constexpr std::string_view kDefaultHttpClientAlias = "default";
struct FetchResponseHeaderAccess;
struct FetchResponseAccess;
}  // namespace ruvia::detail

namespace ruvia {

struct HttpClientConfig {
    // Host name or unbracketed address only; keep the port in port.
    std::pmr::string host;
    // Must be non-zero.
    std::uint16_t port{80};
    bool tls{false};
    // Speak HTTP/2 instead of HTTP/1.1. Over TLS this negotiates ALPN "h2" (and fails the
    // handshake if the peer will not); in cleartext it uses HTTP/2 prior knowledge (RFC 7540
    // section 3.4). A single multiplexed connection is used instead of the HTTP/1.1 connection pool.
    //
    // Only HTTP/1.1 and HTTP/2 are supported. HTTP/3 / QUIC is explicitly NOT supported: there
    // is no h3/QUIC transport, ALPN never offers "h3", and no Alt-Svc "h3" advertisement is
    // acted upon : a request is always sent over TCP (h1.1 or, with http2=true, h2).
    bool http2{false};
    // Override the Host header sent to the upstream (default: host[:port]). Lets a reverse proxy
    // connect to one address (host) while presenting a different Host to the upstream vhost.
    std::pmr::string hostHeader;
    // Upstream TLS options (used only when tls == true).
    struct Tls {
        // Verify the upstream certificate chain against this CA bundle (PEM) instead of the system
        // trust store. Empty = use the system default verify paths.
        std::pmr::string caFile;
        // DANGEROUS: skip all upstream certificate + host-name verification. Only for a trusted
        // network or testing against a self-signed upstream; never against an untrusted network.
        bool insecureSkipVerify{false};
        // Client certificate for mutual TLS. Both must be set together (empty = no client cert).
        std::pmr::string certificateChainFile;
        std::pmr::string privateKeyFile;
        std::pmr::string privateKeyPassword;
        // Override the SNI server name AND the certificate host-name that is verified (default:
        // host). Lets you connect to an IP / internal name but present + verify a public name.
        std::pmr::string sniHost;
    } tlsOptions;
    // Must be greater than zero.
    std::size_t poolSizePerWorker{4};
    // nginx-aligned upstream timeouts (names + inactivity semantics + defaults). Set 0 to disable.
    //   proxyConnectTimeout == nginx proxy_connect_timeout (establishing the connection: DNS
    //                          resolve + TCP connect + TLS handshake)
    //   proxyReadTimeout    == nginx proxy_read_timeout    (inactivity gap between two successive
    //                          reads of the response; resets on each read)
    //   proxySendTimeout    == nginx proxy_send_timeout    (inactivity gap between two successive
    //                          writes of the request; resets on each write)
    // HttpFetchOptions::timeout, when set, overrides proxyReadTimeout/proxySendTimeout for one request.
    std::chrono::milliseconds proxyConnectTimeout{std::chrono::seconds(60)};
    std::chrono::milliseconds proxyReadTimeout{std::chrono::seconds(60)};
    std::chrono::milliseconds proxySendTimeout{std::chrono::seconds(60)};
    // Max time to wait for a free pooled connection (no nginx equivalent). 0 disables.
    std::chrono::milliseconds acquireTimeout{0};
    // Set to 0 to disable the response body limit.
    std::size_t maxResponseBodyBytes{kDefaultMaxBufferedBodyBytes};
};

class FetchResponseHeader final {
public:
    [[nodiscard]] std::string_view name() const noexcept {
        return std::string_view(name_.data(), name_.size());
    }

    [[nodiscard]] std::string_view value() const noexcept {
        return std::string_view(value_.data(), value_.size());
    }

private:
    friend struct detail::FetchResponseHeaderAccess;

    FetchResponseHeader(std::pmr::string n, std::pmr::string v)
        : name_(std::move(n)), value_(std::move(v)) {}

    FetchResponseHeader(
        detail::HttpResolvedPmrResourceTag,
        std::string_view n,
        std::string_view v,
        std::pmr::memory_resource* resource)
        : name_(n.data(), n.size(), resource),
          value_(v.data(), v.size(), resource) {}

    std::pmr::string name_;
    std::pmr::string value_;
};

struct HttpFetchOptions {
    class HeaderInit final {
    public:
        constexpr HeaderInit() noexcept = default;

        constexpr HeaderInit(std::span<const HttpHeaderView> headers) noexcept
            : headers_(headers) {}

        template <std::size_t N>
        constexpr HeaderInit(const HttpHeaderView (&headers)[N]) noexcept
            : headers_(headers, N) {}

        template <std::size_t N>
        constexpr HeaderInit(const std::array<HttpHeaderView, N>& headers) noexcept
            : headers_(headers.data(), headers.size()) {}

        template <typename Allocator>
        HeaderInit(const std::vector<HttpHeaderView, Allocator>&) = delete;

        constexpr HeaderInit(std::initializer_list<HttpHeaderView>) = delete;

        constexpr HeaderInit& operator=(std::span<const HttpHeaderView> headers) noexcept {
            headers_ = headers;
            return *this;
        }

        template <std::size_t N>
        constexpr HeaderInit& operator=(const HttpHeaderView (&headers)[N]) noexcept {
            headers_ = std::span<const HttpHeaderView>(headers, N);
            return *this;
        }

        template <std::size_t N>
        constexpr HeaderInit& operator=(const std::array<HttpHeaderView, N>& headers) noexcept {
            headers_ = std::span<const HttpHeaderView>(headers.data(), headers.size());
            return *this;
        }

        template <typename Allocator>
        HeaderInit& operator=(const std::vector<HttpHeaderView, Allocator>&) = delete;

        HeaderInit& operator=(std::initializer_list<HttpHeaderView>) = delete;

        [[nodiscard]] constexpr operator std::span<const HttpHeaderView>() const noexcept {
            return headers_;
        }

        [[nodiscard]] constexpr auto begin() const noexcept {
            return headers_.begin();
        }

        [[nodiscard]] constexpr auto end() const noexcept {
            return headers_.end();
        }

        [[nodiscard]] constexpr std::size_t size() const noexcept {
            return headers_.size();
        }

        [[nodiscard]] constexpr bool empty() const noexcept {
            return headers_.empty();
        }

    private:
        std::span<const HttpHeaderView> headers_{};
    };

    std::string_view method{"GET"};
    // Borrowed header table; elements and pointed-to strings must remain valid through co_await.
    HeaderInit headers{};
    std::string_view body{};  // borrowed; must remain valid through co_await
    std::chrono::milliseconds timeout{0};
    // Maximum 3xx redirects to follow automatically. Only same-origin redirects (identical
    // scheme, host, and port) are followed; a cross-origin or unparseable Location is returned
    // to the caller as the 3xx response. Set to 0 to disable following entirely.
    std::uint32_t maxRedirects{5};
    // Send "Expect: 100-continue" and wait for the server's interim 100 (Continue) before
    // transmitting the request body -- useful for large bodies a server might reject up front
    // (e.g. 401/413/415). Honored for HTTP/1.1 only (ignored over HTTP/2, whose flow control
    // already provides send backpressure) and only when a body/bodyStream is present. If the
    // server answers with a final status (>= 200) first, the body is not sent and that response
    // is returned; if it stays silent, the body is sent anyway after a short bounded wait so a
    // server that ignores the expectation cannot deadlock the request (RFC 7231 section 5.1.1).
    bool expectContinue{false};
    // Streaming response drivers may decode a single gzip/br/zstd Content-Encoding on the fly.
    // Buffered fetch() always decodes regardless of this flag.
    bool decodeStream{false};
};

class FetchResponse final {
public:
    FetchResponse(const FetchResponse&) = delete;
    FetchResponse& operator=(const FetchResponse&) = delete;
    FetchResponse(FetchResponse&&) noexcept = default;
    FetchResponse& operator=(FetchResponse&&) noexcept = default;

    [[nodiscard]] std::uint16_t status() const noexcept {
        return status_;
    }

    [[nodiscard]] std::span<const FetchResponseHeader> headers() const noexcept {
        return std::span<const FetchResponseHeader>(headers_.data(), headers_.size());
    }

    [[nodiscard]] std::string_view body() const noexcept {
        return std::string_view(body_.data(), body_.size());
    }

private:
    friend struct detail::FetchResponseAccess;

    explicit FetchResponse(std::pmr::memory_resource* resource)
        : FetchResponse(detail::HttpResolvedPmrResourceTag{}, detail::httpPmrResourceOrDefault(resource)) {}

    FetchResponse(detail::HttpResolvedPmrResourceTag, std::pmr::memory_resource* resource)
        : headers_(resource),
          body_(resource) {}

    std::uint16_t status_{0};
    std::pmr::vector<FetchResponseHeader> headers_;
    std::pmr::string body_;
};

namespace detail {

struct HttpClientDefinition final {
    std::pmr::string alias;
    HttpClientConfig config;
};

}  // namespace detail

}  // namespace ruvia
