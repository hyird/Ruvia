#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/HttpProtocolVersion.h"
#include "ruvia/http/HttpRequest.h"

namespace ruvia::detail {

enum class RequestKnownHeader : std::uint8_t {
    kAccept,
    kAcceptEncoding,
    kAccessControlRequestHeaders,
    kAccessControlRequestMethod,
    kAuthorization,
    kConnection,
    kContentEncoding,
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

    [[nodiscard]] static HttpRequest make() noexcept {
        return HttpRequest();
    }

    [[nodiscard]] static constexpr std::size_t knownHeaderSlot(RequestKnownHeader name) noexcept {
        const auto slot = static_cast<std::size_t>(name);
        return slot < kCachedHeaderSlots ? slot : kCachedHeaderSlots;
    }

    [[nodiscard]] static std::string_view knownHeader(
        const HttpRequest& request, RequestKnownHeader name) noexcept {
        const auto slot = knownHeaderSlot(name);
        if (slot >= kCachedHeaderSlots) {
            return {};
        }
        const auto index = request.cachedHeaders_[slot];
        return index != 0 && index <= request.headers_.size()
                   ? request.headers_[index - 1].value()
                   : std::string_view{};
    }

    [[nodiscard]] static bool hasKnownHeader(
        const HttpRequest& request, RequestKnownHeader name) noexcept {
        const auto slot = knownHeaderSlot(name);
        return slot < kCachedHeaderSlots && request.cachedHeaders_[slot] != 0 &&
               request.cachedHeaders_[slot] <= request.headers_.size();
    }

    [[nodiscard]] static std::string_view bodyBytes(const HttpRequest& request) noexcept {
        return request.body_;
    }

    static void reset(HttpRequest& request) noexcept {
        request = make();
    }

    static void setResource(HttpRequest& request, std::pmr::memory_resource* resource) noexcept {
        request.resource_ = resource;
    }

    static void setMethod(HttpRequest& request, std::string_view method) noexcept {
        request.method_ = method;
        request.knownMethod_ = classifyHttpMethod(method);
    }

    static void setTarget(HttpRequest& request, std::string_view target) noexcept {
        request.target_ = target;
    }

    static void setScheme(HttpRequest& request, std::string_view scheme) noexcept {
        request.scheme_ = scheme;
    }

    static void setAuthority(HttpRequest& request, std::string_view authority) noexcept {
        request.authority_ = authority;
    }

    static void setTargetForm(HttpRequest& request, ::ruvia::HttpRequestTargetForm form) noexcept {
        request.targetForm_ = form;
    }

    static void setPath(HttpRequest& request, std::string_view path) noexcept {
        request.path_ = path;
    }

    static void setQueryString(HttpRequest& request, std::string_view queryString) noexcept {
        request.queryString_ = queryString;
    }

    static void setProtocolVersion(
        HttpRequest& request, HttpProtocolVersion protocolVersion) noexcept {
        request.protocolVersion_ = protocolVersion;
    }

    // Call once after syntax validation, with the exact semantic field count.
    // Binding happens before allocation; changing the auxiliary request resource
    // later does not change ownership of an existing descriptor block.
    static void reserveHeaders(HttpRequest& request, std::size_t count) {
        if (count > kMaxHttpHeaderFields || !request.headers_.empty()) {
            throw std::logic_error("invalid request header block reservation");
        }
        request.headers_.reserve(count, request.resource());
    }

    static bool addHeader(HttpRequest& request, HttpHeaderView header) {
        if (request.headers_.size() == kMaxHttpHeaderFields) {
            return false;
        }
        request.headers_.append(header);
        return true;
    }

    static bool addHeader(
        HttpRequest& request, HttpHeaderView header, std::size_t knownSlot) {
        if (!addHeader(request, header)) {
            return false;
        }
        if (knownSlot < kCachedHeaderSlots) {
            request.cachedHeaders_[knownSlot] = static_cast<std::uint8_t>(request.headers_.size());
        }
        return true;
    }

    static void setBody(HttpRequest& request, std::string_view body) noexcept {
        request.body_ = body;
    }
};

static_assert(std::to_underlying(RequestKnownHeader::kUserAgent) + 1 ==
              HttpRequestAccess::kCachedHeaderSlots);

[[nodiscard]] inline std::string_view requestKnownHeader(
    const HttpRequest& request, RequestKnownHeader name) noexcept {
    return HttpRequestAccess::knownHeader(request, name);
}

[[nodiscard]] inline bool requestHasKnownHeader(
    const HttpRequest& request, RequestKnownHeader name) noexcept {
    return HttpRequestAccess::hasKnownHeader(request, name);
}

[[nodiscard]] inline std::string_view requestBodyBytes(const HttpRequest& request) noexcept {
    return HttpRequestAccess::bodyBytes(request);
}

}  // namespace ruvia::detail
