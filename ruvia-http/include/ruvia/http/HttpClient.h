#pragma once

// Outbound HTTP client protocol models.
//
// OWNERSHIP: these are transport-free HTTP values. HttpOrigin and
// HttpClientRequest borrow their string storage; HttpClientResponseHead owns
// parsed header storage through PMR. Response content remains owned by the
// external sans-I/O driver that follows the framing plan. Socket/TLS
// configuration, pools, redirect limits, and timeouts also belong there.

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/HttpProtocolVersion.h"
#include "ruvia/http/detail/PmrResource.h"

namespace ruvia::detail {
struct HttpClientResponseHeaderAccess;
struct HttpClientResponseHeadAccess;
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
    [[nodiscard]] std::string_view name() const & noexcept {
        return std::string_view(name_.data(), name_.size());
    }
    [[nodiscard]] std::string_view name() const && = delete;

    [[nodiscard]] std::string_view value() const & noexcept {
        return std::string_view(value_.data(), value_.size());
    }
    [[nodiscard]] std::string_view value() const && = delete;

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

class HttpClientRequestContent;

class HttpClientRequestWithoutContent final {
private:
    friend class HttpClientRequestContent;

    constexpr HttpClientRequestWithoutContent() noexcept = default;
};

class HttpClientRequestBytes final {
public:
    [[nodiscard]] constexpr std::string_view value() const noexcept {
        return value_;
    }

private:
    friend class HttpClientRequestContent;

    explicit constexpr HttpClientRequestBytes(std::string_view value) noexcept
        : value_(value) {}

    std::string_view value_;
};

// Borrowed outbound request content. `none()` and `bytes("")` are deliberately
// distinct: the latter asks an HTTP/1 writer to emit Content-Length: 0, while
// the former sends no content framing field. Only the active bytes alternative
// exposes a value. The referenced bytes must remain alive and unchanged until
// the external runtime finishes sending them.
class HttpClientRequestContent final {
public:
    [[nodiscard]] static constexpr HttpClientRequestContent none() noexcept {
        return HttpClientRequestContent(HttpClientRequestWithoutContent());
    }

    [[nodiscard]] static constexpr HttpClientRequestContent bytes(
        std::string_view value) noexcept {
        return HttpClientRequestContent(HttpClientRequestBytes(value));
    }

    template <typename Traits, typename Allocator>
    static HttpClientRequestContent bytes(
        std::basic_string<char, Traits, Allocator>&&) = delete;

    template <typename Traits, typename Allocator>
    static HttpClientRequestContent bytes(
        const std::basic_string<char, Traits, Allocator>&&) = delete;

    [[nodiscard]] constexpr const HttpClientRequestWithoutContent*
    withoutContent() const & noexcept {
        return std::get_if<HttpClientRequestWithoutContent>(&content_);
    }
    const HttpClientRequestWithoutContent* withoutContent() const && = delete;

    [[nodiscard]] constexpr const HttpClientRequestBytes*
    borrowedBytes() const & noexcept {
        return std::get_if<HttpClientRequestBytes>(&content_);
    }
    const HttpClientRequestBytes* borrowedBytes() const && = delete;

private:
    using Content = std::variant<
        HttpClientRequestWithoutContent,
        HttpClientRequestBytes>;

    explicit constexpr HttpClientRequestContent(
        HttpClientRequestWithoutContent content) noexcept
        : content_(content) {}

    explicit constexpr HttpClientRequestContent(
        HttpClientRequestBytes content) noexcept
        : content_(content) {}

    Content content_;
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

class HttpClientResponseHead final {
public:
    HttpClientResponseHead(const HttpClientResponseHead&) = delete;
    HttpClientResponseHead& operator=(const HttpClientResponseHead&) = delete;
    HttpClientResponseHead(HttpClientResponseHead&&) noexcept = default;
    HttpClientResponseHead& operator=(HttpClientResponseHead&&) = delete;

    [[nodiscard]] std::uint16_t status() const noexcept {
        return status_;
    }

    [[nodiscard]] HttpProtocolVersion protocolVersion() const noexcept {
        return protocolVersion_;
    }

    [[nodiscard]] std::span<const HttpClientResponseHeader>
    headers() const & noexcept {
        return std::span<const HttpClientResponseHeader>(headers_.data(), headers_.size());
    }
    [[nodiscard]] std::span<const HttpClientResponseHeader>
    headers() const && = delete;

private:
    friend struct detail::HttpClientResponseHeadAccess;

    HttpClientResponseHead(
        std::uint16_t status,
        HttpProtocolVersion protocolVersion,
        std::pmr::memory_resource* resource)
        : HttpClientResponseHead(
              detail::HttpResolvedPmrResourceTag{},
              status,
              protocolVersion,
              detail::httpPmrResourceOrDefault(resource)) {}

    HttpClientResponseHead(
        detail::HttpResolvedPmrResourceTag,
        std::uint16_t status,
        HttpProtocolVersion protocolVersion,
        std::pmr::memory_resource* resource)
        : status_(status),
          protocolVersion_(protocolVersion),
          headers_(resource) {}

    std::uint16_t status_;
    HttpProtocolVersion protocolVersion_;
    std::pmr::vector<HttpClientResponseHeader> headers_;
};

}  // namespace ruvia
