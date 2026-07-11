#pragma once

// Outbound HTTP client protocol models.
//
// OWNERSHIP: these are transport-free HTTP values. HttpOrigin and
// HttpClientRequest borrow their string storage; HttpClientResponse owns parsed
// header/body storage through PMR. Socket/TLS configuration, pools, redirect
// limits, timeouts, and streaming drivers belong to the external I/O owner.

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/http/HttpCommon.h"
#include "ruvia/http/detail/PmrResource.h"

namespace ruvia::detail {
struct HttpClientResponseHeaderAccess;
struct HttpClientResponseAccess;
}  // namespace ruvia::detail

namespace ruvia {

enum class HttpScheme : std::uint8_t {
    kHttp,
    kHttps,
};

class HttpOrigin final {
public:
    // `host` is a borrowed RFC 3986 uri-host; its storage must outlive this
    // value and its bytes must remain unchanged. IP literals therefore include
    // brackets (for example, "[::1]"). Factories reject an empty or malformed
    // host before an origin can be observed.
    [[nodiscard]] static HttpOrigin http(
        std::string_view host,
        std::uint16_t port = 80);

    template <typename Traits, typename Allocator>
    static HttpOrigin http(
        std::basic_string<char, Traits, Allocator>&&,
        std::uint16_t = 80) = delete;

    template <typename Traits, typename Allocator>
    static HttpOrigin http(
        const std::basic_string<char, Traits, Allocator>&&,
        std::uint16_t = 80) = delete;

    [[nodiscard]] static HttpOrigin https(
        std::string_view host,
        std::uint16_t port = 443);

    template <typename Traits, typename Allocator>
    static HttpOrigin https(
        std::basic_string<char, Traits, Allocator>&&,
        std::uint16_t = 443) = delete;

    template <typename Traits, typename Allocator>
    static HttpOrigin https(
        const std::basic_string<char, Traits, Allocator>&&,
        std::uint16_t = 443) = delete;

    [[nodiscard]] constexpr HttpScheme scheme() const noexcept {
        return scheme_;
    }

    // RFC 3986 uri-host only; keep the port in port().
    [[nodiscard]] constexpr std::string_view host() const noexcept {
        return host_;
    }

    [[nodiscard]] constexpr std::uint16_t port() const noexcept {
        return port_;
    }

private:
    constexpr HttpOrigin(
        HttpScheme scheme,
        std::string_view host,
        std::uint16_t port) noexcept
        : host_(host), port_(port), scheme_(scheme) {}

    std::string_view host_;
    std::uint16_t port_;
    HttpScheme scheme_;
};

class HttpClientResponseHeader final {
public:
    [[nodiscard]] std::string_view name() const noexcept {
        return std::string_view(name_.data(), name_.size());
    }

    [[nodiscard]] std::string_view value() const noexcept {
        return std::string_view(value_.data(), value_.size());
    }

private:
    friend struct detail::HttpClientResponseHeaderAccess;

    HttpClientResponseHeader(std::pmr::string name, std::pmr::string value)
        : name_(std::move(name)), value_(std::move(value)) {}

    HttpClientResponseHeader(
        detail::HttpResolvedPmrResourceTag,
        std::string_view name,
        std::string_view value,
        std::pmr::memory_resource* resource)
        : name_(name.data(), name.size(), resource),
          value_(value.data(), value.size(), resource) {}

    std::pmr::string name_;
    std::pmr::string value_;
};

enum class HttpClientRequestContentMode : std::uint8_t {
    kNone,
    kBytes,
};

// Borrowed outbound request content. `none()` and `bytes("")` are deliberately
// distinct: the latter asks an HTTP/1 writer to emit Content-Length: 0, while
// the former sends no content framing field. The referenced bytes must remain
// alive and unchanged until the external runtime finishes sending them.
class HttpClientRequestContent final {
public:
    [[nodiscard]] static constexpr HttpClientRequestContent none() noexcept {
        return HttpClientRequestContent(HttpClientRequestContentMode::kNone, {});
    }

    [[nodiscard]] static constexpr HttpClientRequestContent bytes(
        std::string_view value) noexcept {
        return HttpClientRequestContent(HttpClientRequestContentMode::kBytes, value);
    }

    template <typename Traits, typename Allocator>
    static HttpClientRequestContent bytes(
        std::basic_string<char, Traits, Allocator>&&) = delete;

    template <typename Traits, typename Allocator>
    static HttpClientRequestContent bytes(
        const std::basic_string<char, Traits, Allocator>&&) = delete;

    [[nodiscard]] constexpr HttpClientRequestContentMode mode() const noexcept {
        return mode_;
    }

    [[nodiscard]] constexpr std::string_view value() const noexcept {
        return value_;
    }

private:
    constexpr HttpClientRequestContent(
        HttpClientRequestContentMode mode,
        std::string_view value) noexcept
        : value_(value), mode_(mode) {}

    std::string_view value_;
    HttpClientRequestContentMode mode_;
};

struct HttpClientRequest {
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
    std::string_view target{"/"};
    // Borrowed header table; its elements and their strings must remain alive
    // and unchanged through request preparation and any corresponding HTTP/1
    // response-head decision that inspects the prepared request context.
    HeaderInit headers{};
    HttpClientRequestContent content{HttpClientRequestContent::none()};
};

class HttpClientResponse final {
public:
    HttpClientResponse(const HttpClientResponse&) = delete;
    HttpClientResponse& operator=(const HttpClientResponse&) = delete;
    HttpClientResponse(HttpClientResponse&&) noexcept = default;
    HttpClientResponse& operator=(HttpClientResponse&&) noexcept = default;

    [[nodiscard]] std::uint16_t status() const noexcept {
        return status_;
    }

    [[nodiscard]] HttpProtocolVersion protocolVersion() const noexcept {
        return protocolVersion_;
    }

    [[nodiscard]] std::span<const HttpClientResponseHeader> headers() const noexcept {
        return std::span<const HttpClientResponseHeader>(headers_.data(), headers_.size());
    }

    [[nodiscard]] std::string_view body() const noexcept {
        return std::string_view(body_.data(), body_.size());
    }

private:
    friend struct detail::HttpClientResponseAccess;

    HttpClientResponse(
        HttpProtocolVersion protocolVersion,
        std::pmr::memory_resource* resource)
        : HttpClientResponse(
              detail::HttpResolvedPmrResourceTag{},
              protocolVersion,
              detail::httpPmrResourceOrDefault(resource)) {}

    HttpClientResponse(
        detail::HttpResolvedPmrResourceTag,
        HttpProtocolVersion protocolVersion,
        std::pmr::memory_resource* resource)
        : protocolVersion_(protocolVersion),
          headers_(resource),
          body_(resource) {}

    std::uint16_t status_{0};
    HttpProtocolVersion protocolVersion_;
    std::pmr::vector<HttpClientResponseHeader> headers_;
    std::pmr::string body_;
};

}  // namespace ruvia
