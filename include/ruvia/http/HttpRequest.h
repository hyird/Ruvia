#pragma once

#include "ruvia/http/HttpCommon.h"

#include <array>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace ruvia {

class Context;
class HttpParser;

namespace detail {

class HttpServer;
class Http2RequestBuilder;

}  // namespace detail

class HttpRequest final {
public:
    enum class KnownHeader : std::uint8_t {
        kAccept,
        kAcceptEncoding,
        kAccessControlRequestHeaders,
        kAccessControlRequestMethod,
        kAuthorization,
        kConnection,
        kContentLength,
        kContentType,
        kCookie,
        kExpect,
        kHost,
        kIfMatch,
        kIfModifiedSince,
        kIfNoneMatch,
        kIfRange,
        kIfUnmodifiedSince,
        kOrigin,
        kRange,
        kSecWebSocketKey,
        kSecWebSocketProtocol,
        kSecWebSocketVersion,
        kTransferEncoding,
        kUpgrade,
        kUserAgent,
    };

    [[nodiscard]] HttpMethod method() const noexcept {
        return method_;
    }

    [[nodiscard]] std::string_view target() const noexcept {
        return target_;
    }

    [[nodiscard]] std::string_view path() const noexcept {
        return path_;
    }

    [[nodiscard]] std::optional<std::pmr::string> decodedPath() const;

    [[nodiscard]] std::string_view queryString() const noexcept {
        return queryString_;
    }

    [[nodiscard]] std::string_view httpVersion() const noexcept {
        return httpVersion_;
    }

    [[nodiscard]] std::span<const HttpHeaderView> headers() const noexcept {
        return std::span<const HttpHeaderView>(headers_.data(), headerCount_);
    }

    [[nodiscard]] std::string_view header(std::string_view name) const noexcept;
    [[nodiscard]] std::string_view header(KnownHeader name) const noexcept;
    [[nodiscard]] QueryValue query(std::string_view name) const noexcept;
    [[nodiscard]] std::optional<std::string_view> cookie(std::string_view name) const noexcept;
    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept;

    [[nodiscard]] std::string_view remoteAddress() const noexcept {
        return remoteAddress_;
    }

private:
    friend class Context;
    friend class HttpParser;
    friend class detail::HttpServer;
    friend class detail::Http2RequestBuilder;

    enum KnownRequestHeaderBit : std::uint32_t {
        kKnownHeaderAccept                      = 1U << 0,
        kKnownHeaderAcceptEncoding              = 1U << 1,
        kKnownHeaderAccessControlRequestHeaders = 1U << 2,
        kKnownHeaderAccessControlRequestMethod  = 1U << 3,
        kKnownHeaderConnection                  = 1U << 4,
        kKnownHeaderContentLength               = 1U << 5,
        kKnownHeaderContentType                 = 1U << 6,
        kKnownHeaderCookie                      = 1U << 7,
        kKnownHeaderExpect                      = 1U << 8,
        kKnownHeaderHost                        = 1U << 9,
        kKnownHeaderIfMatch                     = 1U << 10,
        kKnownHeaderIfModifiedSince             = 1U << 11,
        kKnownHeaderIfNoneMatch                 = 1U << 12,
        kKnownHeaderIfRange                     = 1U << 13,
        kKnownHeaderIfUnmodifiedSince           = 1U << 14,
        kKnownHeaderOrigin                      = 1U << 15,
        kKnownHeaderRange                       = 1U << 16,
        kKnownHeaderSecWebSocketKey             = 1U << 17,
        kKnownHeaderSecWebSocketProtocol        = 1U << 18,
        kKnownHeaderSecWebSocketVersion         = 1U << 19,
        kKnownHeaderTransferEncoding            = 1U << 20,
        kKnownHeaderUpgrade                     = 1U << 21,
        kKnownHeaderAuthorization               = 1U << 22,
        kKnownHeaderUserAgent                   = 1U << 23,
    };

    static constexpr std::size_t kKnownRequestHeaderCount =
        static_cast<std::size_t>(KnownHeader::kUserAgent) + 1;
    static constexpr std::array<std::uint32_t, kKnownRequestHeaderCount> kKnownHeaderBitsBySlot{
        kKnownHeaderAccept,
        kKnownHeaderAcceptEncoding,
        kKnownHeaderAccessControlRequestHeaders,
        kKnownHeaderAccessControlRequestMethod,
        kKnownHeaderAuthorization,
        kKnownHeaderConnection,
        kKnownHeaderContentLength,
        kKnownHeaderContentType,
        kKnownHeaderCookie,
        kKnownHeaderExpect,
        kKnownHeaderHost,
        kKnownHeaderIfMatch,
        kKnownHeaderIfModifiedSince,
        kKnownHeaderIfNoneMatch,
        kKnownHeaderIfRange,
        kKnownHeaderIfUnmodifiedSince,
        kKnownHeaderOrigin,
        kKnownHeaderRange,
        kKnownHeaderSecWebSocketKey,
        kKnownHeaderSecWebSocketProtocol,
        kKnownHeaderSecWebSocketVersion,
        kKnownHeaderTransferEncoding,
        kKnownHeaderUpgrade,
        kKnownHeaderUserAgent};

    [[nodiscard]] static constexpr std::size_t knownHeaderSlot(KnownHeader name) noexcept {
        const auto slot = static_cast<std::size_t>(name);
        return slot < kKnownRequestHeaderCount ? slot : kKnownRequestHeaderCount;
    }

    void setKnownHeader(KnownHeader name, std::string_view value) noexcept {
        const auto slot = knownHeaderSlot(name);
        if (slot >= kKnownRequestHeaderCount) {
            return;
        }
        knownHeaders_[slot] = value;
        knownHeaderBits_ |= kKnownHeaderBitsBySlot[slot];
    }

    void setMethod(HttpMethod method) noexcept {
        method_ = method;
    }

    void setTarget(std::string_view target) noexcept {
        target_ = target;
    }

    void setPath(std::string_view path) noexcept {
        path_ = path;
    }

    void setQueryString(std::string_view queryString) noexcept {
        queryString_ = queryString;
    }

    void setHttpVersion(std::string_view httpVersion) noexcept {
        httpVersion_ = httpVersion;
    }

    bool addHeader(HttpHeaderView header) noexcept {
        if (headerCount_ == kMaxRequestHeaders) {
            return false;
        }
        headers_[headerCount_++] = header;
        return true;
    }

    void setBody(std::string_view body) noexcept {
        body_ = body;
    }

    void setConnectionHeader(std::string_view value) noexcept {
        setKnownHeader(KnownHeader::kConnection, value);
    }

    void setAuthorizationHeader(std::string_view value) noexcept {
        setKnownHeader(KnownHeader::kAuthorization, value);
    }

    void setHostHeader(std::string_view value) noexcept {
        setKnownHeader(KnownHeader::kHost, value);
    }

    void setContentLengthHeader(std::string_view value) noexcept {
        setKnownHeader(KnownHeader::kContentLength, value);
    }

    void setTransferEncodingHeader(std::string_view value) noexcept {
        setKnownHeader(KnownHeader::kTransferEncoding, value);
    }

    void setExpectHeader(std::string_view value) noexcept {
        setKnownHeader(KnownHeader::kExpect, value);
    }

    void setContentTypeHeader(std::string_view value) noexcept {
        setKnownHeader(KnownHeader::kContentType, value);
    }

    void setCookieHeader(std::string_view value) noexcept {
        setKnownHeader(KnownHeader::kCookie, value);
    }

    void setOriginHeader(std::string_view value) noexcept {
        setKnownHeader(KnownHeader::kOrigin, value);
    }

    void setAccessControlRequestMethodHeader(std::string_view value) noexcept {
        setKnownHeader(KnownHeader::kAccessControlRequestMethod, value);
    }

    void setAccessControlRequestHeadersHeader(std::string_view value) noexcept {
        setKnownHeader(KnownHeader::kAccessControlRequestHeaders, value);
    }

    void setAcceptEncodingHeader(std::string_view value) noexcept {
        setKnownHeader(KnownHeader::kAcceptEncoding, value);
    }

    void setAcceptHeader(std::string_view value) noexcept {
        setKnownHeader(KnownHeader::kAccept, value);
    }

    void setRangeHeader(std::string_view value) noexcept {
        setKnownHeader(KnownHeader::kRange, value);
    }

    void setIfMatchHeader(std::string_view value) noexcept {
        setKnownHeader(KnownHeader::kIfMatch, value);
    }

    void setIfNoneMatchHeader(std::string_view value) noexcept {
        setKnownHeader(KnownHeader::kIfNoneMatch, value);
    }

    void setIfModifiedSinceHeader(std::string_view value) noexcept {
        setKnownHeader(KnownHeader::kIfModifiedSince, value);
    }

    void setIfUnmodifiedSinceHeader(std::string_view value) noexcept {
        setKnownHeader(KnownHeader::kIfUnmodifiedSince, value);
    }

    void setIfRangeHeader(std::string_view value) noexcept {
        setKnownHeader(KnownHeader::kIfRange, value);
    }

    void setUpgradeHeader(std::string_view value) noexcept {
        setKnownHeader(KnownHeader::kUpgrade, value);
    }

    void setSecWebSocketKeyHeader(std::string_view value) noexcept {
        setKnownHeader(KnownHeader::kSecWebSocketKey, value);
    }

    void setSecWebSocketVersionHeader(std::string_view value) noexcept {
        setKnownHeader(KnownHeader::kSecWebSocketVersion, value);
    }

    void setSecWebSocketProtocolHeader(std::string_view value) noexcept {
        setKnownHeader(KnownHeader::kSecWebSocketProtocol, value);
    }

    void setUserAgentHeader(std::string_view value) noexcept {
        setKnownHeader(KnownHeader::kUserAgent, value);
    }

    void setResource(std::pmr::memory_resource* resource) noexcept {
        resource_ = resource;
    }

    void setRemoteAddress(std::string_view remoteAddress) noexcept {
        remoteAddress_ = remoteAddress;
    }

    void reset() noexcept {
        method_ = HttpMethod::kUnknown;
        target_ = {};
        path_ = {};
        queryString_ = {};
        httpVersion_ = "HTTP/1.1";
        headerCount_ = 0;
        knownHeaderBits_ = 0;
        knownHeaders_.fill(std::string_view{});
        body_ = {};
        remoteAddress_ = {};
        resource_ = nullptr;
    }

    HttpMethod method_{HttpMethod::kUnknown};
    std::string_view target_;
    std::string_view path_;
    std::string_view queryString_;
    std::string_view httpVersion_{"HTTP/1.1"};
    std::array<HttpHeaderView, kMaxRequestHeaders> headers_{};
    std::size_t headerCount_{0};
    std::uint32_t knownHeaderBits_{0};
    std::array<std::string_view, kKnownRequestHeaderCount> knownHeaders_{};
    std::string_view body_;
    std::string_view remoteAddress_;
    std::pmr::memory_resource* resource_{nullptr};
};

}  // namespace ruvia
