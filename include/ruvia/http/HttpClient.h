#pragma once

#ifdef RUVIA_ENABLE_HTTP_CLIENT

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/app/Task.h"
#include "ruvia/http/HttpLimits.h"
#include "ruvia/memory/PmrResource.h"

namespace ruvia::detail {
inline constexpr std::string_view kDefaultHttpClientAlias = "default";
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
    bool http2{false};
    // Must be greater than zero.
    std::size_t poolSizePerWorker{4};
    // Set to 0 to disable the corresponding timeout.
    std::chrono::milliseconds connectTimeout{0};
    std::chrono::milliseconds requestTimeout{0};
    std::chrono::milliseconds acquireTimeout{0};
    // Set to 0 to disable the response body limit.
    std::size_t maxResponseBodyBytes{kDefaultMaxBufferedBodyBytes};
};

struct FetchRequestHeader {
    std::string_view name;   // borrowed; must remain valid through co_await
    std::string_view value;  // borrowed; must remain valid through co_await
};

struct FetchResponseHeader {
    std::pmr::string name;
    std::pmr::string value;

    FetchResponseHeader() = default;

    FetchResponseHeader(std::pmr::string n, std::pmr::string v)
        : name(std::move(n)), value(std::move(v)) {}

    FetchResponseHeader(std::string_view n, std::string_view v, std::pmr::memory_resource* resource)
        : FetchResponseHeader(detail::ResolvedPmrResourceTag{}, n, v, detail::pmrResourceOrDefault(resource)) {}

private:
    FetchResponseHeader(
        detail::ResolvedPmrResourceTag,
        std::string_view n,
        std::string_view v,
        std::pmr::memory_resource* resource)
        : name(n.data(), n.size(), resource),
          value(v.data(), v.size(), resource) {}
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

    [[nodiscard]] constexpr bool valid() const noexcept { return nextChunk_ != nullptr; }
    [[nodiscard]] Task<std::string_view> nextChunk() const { return nextChunk_(target_); }

private:
    void* target_{nullptr};
    NextChunk nextChunk_{nullptr};
};

struct FetchOptions {
    std::string_view method{"GET"};
    // Borrowed header table; elements and pointed-to strings must remain valid through co_await.
    std::span<const FetchRequestHeader> headers{};
    std::string_view body{};  // borrowed; must remain valid through co_await
    // If set, streams the request body from this producer instead of `body` (which must be empty).
    // Borrowed; the target and returned chunk views must outlive their awaited write.
    RequestBodyStream bodyStream{};
    std::chrono::milliseconds timeout{0};
    // Maximum 3xx redirects to follow automatically. Only same-origin redirects (identical
    // scheme, host, and port) are followed; a cross-origin or unparseable Location is returned
    // to the caller as the 3xx response. Set to 0 to disable following entirely.
    std::uint32_t maxRedirects{5};
};

class FetchResponse final {
public:
    explicit FetchResponse(std::pmr::memory_resource* resource = nullptr)
        : FetchResponse(detail::ResolvedPmrResourceTag{}, detail::pmrResourceOrDefault(resource)) {}

    FetchResponse(const FetchResponse&) = delete;
    FetchResponse& operator=(const FetchResponse&) = delete;
    FetchResponse(FetchResponse&&) noexcept = default;
    FetchResponse& operator=(FetchResponse&&) noexcept = default;

    int statusCode{0};
    std::pmr::vector<FetchResponseHeader> headers;
    std::pmr::string body;

private:
    FetchResponse(detail::ResolvedPmrResourceTag, std::pmr::memory_resource* resource)
        : headers(resource),
          body(resource) {}
};

namespace detail {

struct HttpClientDefinition final {
    std::pmr::string alias;
    HttpClientConfig config;
};

class FetchStreamSource;

// Deleter for the pimpl: defers to the source's PMR-aware destroy() (declared here, defined in
// the .cpp where FetchStreamSource is complete).
struct FetchStreamSourceDeleter final {
    void operator()(FetchStreamSource* source) const noexcept;
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
// Note: unlike fetch(), a streamed body is delivered as received — a Content-Encoding is NOT
// transparently decoded (the caller sees the encoded bytes and any Content-Encoding header).
class FetchResponseStream final {
public:
    FetchResponseStream() noexcept = default;
    explicit FetchResponseStream(
        std::unique_ptr<detail::FetchStreamSource, detail::FetchStreamSourceDeleter> source) noexcept;

    FetchResponseStream(const FetchResponseStream&) = delete;
    FetchResponseStream& operator=(const FetchResponseStream&) = delete;
    FetchResponseStream(FetchResponseStream&&) noexcept;
    FetchResponseStream& operator=(FetchResponseStream&&) noexcept;
    ~FetchResponseStream();

    [[nodiscard]] int statusCode() const noexcept;
    [[nodiscard]] std::span<const FetchResponseHeader> headers() const noexcept;
    // Next slice of the body; an empty string signals end of stream. Throws on transport error.
    [[nodiscard]] Task<std::pmr::string> readChunk();
    // Release the connection/stream before the body is fully consumed.
    void close() noexcept;

    [[nodiscard]] explicit operator bool() const noexcept { return source_ != nullptr; }

private:
    std::unique_ptr<detail::FetchStreamSource, detail::FetchStreamSourceDeleter> source_;
};

}  // namespace ruvia

#endif  // RUVIA_ENABLE_HTTP_CLIENT
