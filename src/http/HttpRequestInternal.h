#pragma once

#include "ruvia/http/HttpRequest.h"

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string_view>

namespace ruvia::detail {

enum class RequestKnownHeader : std::uint8_t {
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

struct HttpRequestAccess final {
    static constexpr std::size_t kCachedHeaderSlots = HttpRequest::kCachedHeaderSlots;

    [[nodiscard]] static constexpr std::size_t knownHeaderSlot(RequestKnownHeader name) noexcept {
        const auto slot = static_cast<std::size_t>(name);
        return slot < kCachedHeaderSlots
            ? slot
            : kCachedHeaderSlots;
    }

    [[nodiscard]] static std::string_view knownHeader(
        const HttpRequest& request,
        RequestKnownHeader name) noexcept {
        const auto slot = knownHeaderSlot(name);
        if (slot >= kCachedHeaderSlots) {
            return {};
        }
        return (request.cachedHeaderBits_ & cachedHeaderBit(slot)) != 0
            ? request.cachedHeaders_[slot]
            : std::string_view{};
    }

    [[nodiscard]] static std::string_view bodyBytes(const HttpRequest& request) noexcept {
        return request.body_;
    }

    static void reset(HttpRequest& request) noexcept {
        request.method_ = HttpMethod::kUnknown;
        request.target_ = {};
        request.path_ = {};
        request.queryString_ = {};
        request.httpVersion_ = "HTTP/1.1";
        request.headerCount_ = 0;
        request.cachedHeaderBits_ = 0;
        request.body_ = {};
        request.remoteAddress_ = {};
        request.clientCertificate_ = {};
        request.resource_ = nullptr;
    }

    static void setResource(HttpRequest& request, std::pmr::memory_resource* resource) noexcept {
        request.resource_ = resource;
    }

    static void setMethod(HttpRequest& request, HttpMethod method) noexcept {
        request.method_ = method;
    }

    static void setTarget(HttpRequest& request, std::string_view target) noexcept {
        request.target_ = target;
    }

    static void setPath(HttpRequest& request, std::string_view path) noexcept {
        request.path_ = path;
    }

    static void setQueryString(HttpRequest& request, std::string_view queryString) noexcept {
        request.queryString_ = queryString;
    }

    static void setHttpVersion(HttpRequest& request, std::string_view httpVersion) noexcept {
        request.httpVersion_ = httpVersion;
    }

    static bool addHeader(HttpRequest& request, HttpHeaderView header) noexcept {
        if (request.headerCount_ == kMaxRequestHeaders) {
            return false;
        }
        request.headers_[request.headerCount_++] = header;
        return true;
    }

    static void setKnownHeaderSlot(
        HttpRequest& request,
        std::size_t slot,
        std::string_view value) noexcept {
        if (slot >= kCachedHeaderSlots) {
            return;
        }
        request.cachedHeaders_[slot] = value;
        request.cachedHeaderBits_ |= cachedHeaderBit(slot);
    }

    static void setBody(HttpRequest& request, std::string_view body) noexcept {
        request.body_ = body;
    }

    static void setRemoteAddress(HttpRequest& request, std::string_view remoteAddress) noexcept {
        request.remoteAddress_ = remoteAddress;
    }

    static void setClientCertificate(HttpRequest& request, std::string_view clientCertificate) noexcept {
        request.clientCertificate_ = clientCertificate;
    }

private:
    [[nodiscard]] static constexpr std::uint32_t cachedHeaderBit(std::size_t slot) noexcept {
        return 1U << slot;
    }
};

static_assert(
    static_cast<std::size_t>(RequestKnownHeader::kUserAgent) + 1 ==
    HttpRequestAccess::kCachedHeaderSlots);

[[nodiscard]] inline std::string_view requestKnownHeader(
    const HttpRequest& request,
    RequestKnownHeader name) noexcept {
    return HttpRequestAccess::knownHeader(request, name);
}

[[nodiscard]] inline std::string_view requestBodyBytes(const HttpRequest& request) noexcept {
    return HttpRequestAccess::bodyBytes(request);
}

}  // namespace ruvia::detail
