#pragma once

// Outbound HTTP client protocol models (origin, request options, responses).
//
// OWNERSHIP: these are pure HTTP models and policies. They contain no socket/TLS
// runtime configuration, connection pools, file paths, or clocks. Callers provide
// their own I/O driver and use the shared sans-I/O protocol primitives.

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/http/HttpCommon.h"
#include "ruvia/http/detail/PmrResource.h"

namespace ruvia::detail {
struct FetchResponseHeaderAccess;
struct FetchResponseAccess;
}  // namespace ruvia::detail

namespace ruvia {

struct HttpOrigin {
    // Host name or unbracketed IP address only; keep the port in port.
    std::pmr::string host;
    // Must be non-zero.
    std::uint16_t port{80};
    // URI scheme: false = http, true = https. TLS mechanics belong to the caller's
    // transport driver and intentionally are not represented here.
    bool tls{false};
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
    // Maximum 3xx redirects to follow automatically. Only same-origin redirects (identical
    // scheme, host, and port) are followed; a cross-origin or unparseable Location is returned
    // to the caller as the 3xx response. Set to 0 to disable following entirely.
    std::uint32_t maxRedirects{5};
    // Send "Expect: 100-continue" and wait for the server's interim 100 (Continue) before
    // transmitting the request body -- useful for large bodies a server might reject up front
    // (e.g. 401/413/415). Honored for HTTP/1.1 only (ignored over HTTP/2, whose flow control
    // already provides send backpressure) and only when a body/bodyStream is present. If the
    // server answers with a final status (>= 200) first, the body is not sent. The caller's
    // runtime owns any wait timeout; this flag only expresses the wire-level request policy.
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

}  // namespace ruvia
