#pragma once

// Outbound HTTP client PUBLIC SURFACE (configuration, fetch options, responses).
//
// OWNERSHIP: this API ships with ruvia::web. The client RUNTIME (connection pools,
// TLS, timeouts, redirects, deadlines -- HttpClientPool / Http2ClientSession /
// HttpClientRegistry in ruvia-web/src/client/) is web-layer I/O + policy, surfaced
// through Context::fetch / Context::fetchStream / Context::proxy. ruvia::http alone
// deliberately contains NO ready-to-use client runtime; what it provides is the
// PROTOCOL ENGINE a client is built from (the shared sans-I/O Http2Connection in
// client role, the h1 parser, and the message model), which any runtime can drive.

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/app/Task.h"
#include "ruvia/http/HttpBodyStream.h"
#include "ruvia/http/HttpCommon.h"
#include "ruvia/http/HttpLimits.h"
#include "ruvia/memory/PmrResource.h"

namespace ruvia::detail {
inline constexpr std::string_view kDefaultHttpClientAlias = "default";
struct FetchResponseHeaderAccess;
struct FetchResponseAccess;
struct FetchResponseStreamAccess;
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
    // §3.4). A single multiplexed connection is used instead of the HTTP/1.1 connection pool.
    //
    // Only HTTP/1.1 and HTTP/2 are supported. HTTP/3 / QUIC is explicitly NOT supported: there
    // is no h3/QUIC transport, ALPN never offers "h3", and no Alt-Svc "h3" advertisement is
    // acted upon — a request is always sent over TCP (h1.1 or, with http2=true, h2).
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
    // FetchOptions::timeout, when set, overrides proxyReadTimeout/proxySendTimeout for one request.
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
        detail::ResolvedPmrResourceTag,
        std::string_view n,
        std::string_view v,
        std::pmr::memory_resource* resource)
        : name_(n.data(), n.size(), resource),
          value_(v.data(), v.size(), resource) {}

    std::pmr::string name_;
    std::pmr::string value_;
};

// Borrowed streaming request body producer. The client calls nextChunk(target) repeatedly and
// sends each returned slice as it arrives (HTTP/1.1: chunked transfer-encoding; HTTP/2: DATA
// frames with flow control), stopping on an empty view. The returned view must remain valid until
// that nextChunk() result has been written; it may be invalidated by the next call.
class RequestBodyStream {
public:
    using NextChunk = Task<std::string_view> (*)(void*);

    constexpr RequestBodyStream() noexcept = default;
    constexpr RequestBodyStream(void* target, NextChunk nextChunk) noexcept
        : target_(target),
          nextChunk_(nextChunk) {}

    [[nodiscard]] constexpr explicit operator bool() const noexcept { return nextChunk_ != nullptr; }
    [[nodiscard]] Task<std::string_view> nextChunk() const {
        if (nextChunk_ == nullptr) {
            co_return std::string_view{};
        }
        co_return co_await nextChunk_(target_);
    }

private:
    void* target_{nullptr};
    NextChunk nextChunk_{nullptr};
};

struct FetchOptions {
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
    // If set, streams the request body from this producer instead of `body` (which must be empty).
    // Borrowed; the target and returned chunk views must outlive their awaited write.
    RequestBodyStream bodyStream{};
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
    // server that ignores the expectation cannot deadlock the request (RFC 7231 §5.1.1).
    bool expectContinue{false};
    // Streaming responses only (Context::fetchStream / FetchResponseStream): decode a single
    // gzip/br/zstd Content-Encoding on the fly so readChunk() yields decoded bytes. Off by
    // default, so a streamed body is delivered as received unless requested. A buffered fetch()
    // always decodes regardless of this flag. Ignored (no-op) for a multi-coding or unknown
    // Content-Encoding, which is passed through as received.
    bool decodeStream{false};
};

// Options for Context::proxy (Hono-style reverse proxy). Mirrors hono/proxy: the incoming request
// is forwarded to the upstream and the upstream's response is streamed straight back.
struct ProxyOptions {
    // Forward the incoming request headers to the upstream (default true). Hop-by-hop and
    // client-managed headers (Host, Connection, Content-Length, TE, Transfer-Encoding, ...) are
    // always dropped -- the client sets them itself.
    bool forwardRequestHeaders{true};
    // Maximum 3xx redirects to follow on the upstream. 0 (default) passes a 3xx straight back to
    // the downstream client rather than following it.
    std::uint32_t maxRedirects{0};
    // Overrides the client's proxy_read_timeout / proxy_send_timeout for this request. 0 = use the
    // client config's values.
    std::chrono::milliseconds timeout{0};
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
        : FetchResponse(detail::ResolvedPmrResourceTag{}, detail::pmrResourceOrDefault(resource)) {}

    FetchResponse(detail::ResolvedPmrResourceTag, std::pmr::memory_resource* resource)
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

// A streamed response body. Obtained from Context::fetchStream. The status line and headers are
// available immediately; readChunk() pulls the body incrementally (empty result = end of stream),
// giving backpressure and bounded memory for large downloads or long-lived responses. Move-only;
// destruction (or close()) releases the underlying connection/stream.
//
// Lifetime: the stream holds the underlying HTTP client connection open and refers back to the
// client, so it must be fully consumed or closed (and destroyed) before the App / HTTP client it
// came from is torn down — do not retain one past the request that produced it. It is also single-
// consumer: readChunk()/close() must be driven from one coroutine, not concurrently.
//
// Note: unlike fetch(), a streamed body is by default delivered as received — a Content-Encoding
// is NOT transparently decoded (the caller sees the encoded bytes). Set FetchOptions::decodeStream
// to decode a single gzip/br/zstd coding on the fly; the Content-Encoding header is still present.
class FetchResponseStream final {
public:
    FetchResponseStream(const FetchResponseStream&) = delete;
    FetchResponseStream& operator=(const FetchResponseStream&) = delete;
    FetchResponseStream(FetchResponseStream&&) noexcept = default;
    FetchResponseStream& operator=(FetchResponseStream&&) noexcept = default;
    ~FetchResponseStream() = default;

    [[nodiscard]] std::uint16_t status() const noexcept { return status_; }
    [[nodiscard]] std::span<const FetchResponseHeader> headers() const noexcept {
        return std::span<const FetchResponseHeader>(headers_.data(), headers_.size());
    }
    // Next slice of the body; an empty view signals end of stream (the view is valid until the next
    // readChunk()/close()). Throws on transport error.
    [[nodiscard]] Task<std::string_view> readChunk() { return body_.nextChunk(); }
    // Release the connection/stream before the body is fully consumed.
    void close() noexcept { body_ = HttpBodyStream{}; }

    [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(body_); }

    // Hand off the underlying pull stream (e.g. so a reverse proxy can attach it to an
    // HttpResponse as its streaming body). Leaves this stream empty.
    [[nodiscard]] HttpBodyStream takeBody() noexcept { return std::move(body_); }

private:
    friend struct detail::FetchResponseStreamAccess;

    FetchResponseStream() noexcept = default;

    FetchResponseStream(
        std::uint16_t status,
        std::pmr::vector<FetchResponseHeader> headers,
        HttpBodyStream body) noexcept
        : status_(status), headers_(std::move(headers)), body_(std::move(body)) {}

    std::uint16_t status_{0};
    std::pmr::vector<FetchResponseHeader> headers_;
    HttpBodyStream body_;
};

namespace detail {

}  // namespace detail

}  // namespace ruvia
