#pragma once
#include <span>
#include "ruvia/http/HttpResponse.h"

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string_view>
#include <utility>

namespace ruvia::detail {

struct HttpResponseHeaderStateAccess final {
    [[nodiscard]] static HttpResponse cloneForTransaction(const HttpResponse& response) {
        return response.cloneForTransaction();
    }

    static void setStableView(
        HttpResponse& response, std::string_view key, std::string_view value) {
        response.setHeaderStableView(key, value);
    }

    static void setValidated(HttpResponse& response, std::string_view key, std::string_view value,
        std::uint32_t knownBit) {
        response.setHeaderValidated(key, value, knownBit);
    }

    static void appendValidated(HttpResponse& response, std::string_view key,
        std::string_view value, std::uint32_t knownBit) {
        response.appendHeaderValidated(key, value, knownBit);
    }

    [[nodiscard]] static HttpResponseHeader& upsertSetCookieUninitializedValue(
        HttpResponse& response, std::string_view wirePrefix, std::string_view cookieName,
        std::string_view path, std::string_view domain, std::size_t valueSize) {
        return response.upsertSetCookieHeaderUninitializedValue(
            wirePrefix, cookieName, path, domain, valueSize);
    }

    static void upsertSetCookieValidated(HttpResponse& response, std::string_view value) {
        response.upsertSetCookieHeaderValidated(value);
    }

    static void setUnsigned(
        HttpResponse& response, std::string_view key, std::uint64_t value, std::uint32_t knownBit) {
        response.setHeaderUnsigned(key, value, knownBit);
    }

    static void setAllow(HttpResponse& response, std::uint32_t methodMask,
        std::span<const std::string_view> extensionMethods = {}) {
        response.setAllowHeader(methodMask, extensionMethods);
    }

    static void setContentRange(
        HttpResponse& response, std::uint64_t offset, std::uint64_t length, std::uint64_t size) {
        response.setContentRange(offset, length, size);
    }

    static void setContentRangeUnsatisfied(HttpResponse& response, std::uint64_t size) {
        response.setContentRangeUnsatisfied(size);
    }

    static void reserve(HttpResponse& response, std::size_t count) {
        response.reserveHeaders(count);
    }

    static void replaceBodyWithContentEncoding(
        HttpResponse& response, std::pmr::string&& value, std::string_view contentEncoding) {
        response.replaceBodyWithContentEncoding(std::move(value), contentEncoding);
    }

    [[nodiscard]] static std::uint32_t knownBits(const HttpResponse& response) noexcept {
        return response.knownHeaderBits_;
    }

    [[nodiscard]] static bool hasKnown(const HttpResponse& response, std::uint32_t bit) noexcept {
        return (response.knownHeaderBits_ & bit) != 0;
    }

    [[nodiscard]] static std::string_view knownValue(
        const HttpResponse& response, std::uint32_t bit) noexcept {
        return response.knownHeaderValue(bit);
    }

    [[nodiscard]] static std::pmr::memory_resource* resource(
        const HttpResponse& response) noexcept {
        return response.resource();
    }
};

inline void setResponseHeaderStableView(
    HttpResponse& response, std::string_view key, std::string_view value) {
    HttpResponseHeaderStateAccess::setStableView(response, key, value);
}

inline void setResponseHeaderValidated(
    HttpResponse& response, std::string_view key, std::string_view value, std::uint32_t knownBit) {
    HttpResponseHeaderStateAccess::setValidated(response, key, value, knownBit);
}

inline void appendResponseHeaderValidated(
    HttpResponse& response, std::string_view key, std::string_view value, std::uint32_t knownBit) {
    HttpResponseHeaderStateAccess::appendValidated(response, key, value, knownBit);
}

[[nodiscard]] inline HttpResponseHeader& upsertResponseSetCookieUninitializedValue(
    HttpResponse& response, std::string_view wirePrefix, std::string_view cookieName,
    std::string_view path, std::string_view domain, std::size_t valueSize) {
    return HttpResponseHeaderStateAccess::upsertSetCookieUninitializedValue(
        response, wirePrefix, cookieName, path, domain, valueSize);
}

inline void upsertResponseSetCookieValidated(HttpResponse& response, std::string_view value) {
    HttpResponseHeaderStateAccess::upsertSetCookieValidated(response, value);
}

inline void setResponseHeaderUnsigned(
    HttpResponse& response, std::string_view key, std::uint64_t value, std::uint32_t knownBit) {
    HttpResponseHeaderStateAccess::setUnsigned(response, key, value, knownBit);
}

inline void setResponseAllowHeader(HttpResponse& response, std::uint32_t methodMask,
    std::span<const std::string_view> extensionMethods = {}) {
    HttpResponseHeaderStateAccess::setAllow(response, methodMask, extensionMethods);
}

inline void setResponseContentRange(
    HttpResponse& response, std::uint64_t offset, std::uint64_t length, std::uint64_t size) {
    HttpResponseHeaderStateAccess::setContentRange(response, offset, length, size);
}

inline void setResponseContentRangeUnsatisfied(HttpResponse& response, std::uint64_t size) {
    HttpResponseHeaderStateAccess::setContentRangeUnsatisfied(response, size);
}

inline void reserveResponseHeaders(HttpResponse& response, std::size_t count) {
    HttpResponseHeaderStateAccess::reserve(response, count);
}

// Atomically prepares the three representation fields affected by buffered
// content-coding (Content-Encoding, Content-Length and a strong ETag's weak
// replacement) before publishing the owned body. Web compression uses this
// boundary so an allocation failure cannot leave identity bytes carrying
// compressed metadata, or compressed bytes carrying an identity length.
inline void replaceResponseBodyWithContentEncoding(
    HttpResponse& response, std::pmr::string&& value, std::string_view contentEncoding) {
    HttpResponseHeaderStateAccess::replaceBodyWithContentEncoding(
        response, std::move(value), contentEncoding);
}

[[nodiscard]] inline std::uint32_t responseKnownHeaderBits(const HttpResponse& response) noexcept {
    return HttpResponseHeaderStateAccess::knownBits(response);
}

[[nodiscard]] inline bool responseHasKnownHeader(
    const HttpResponse& response, std::uint32_t bit) noexcept {
    return HttpResponseHeaderStateAccess::hasKnown(response, bit);
}

[[nodiscard]] inline std::string_view responseKnownHeader(
    const HttpResponse& response, std::uint32_t bit) noexcept {
    return HttpResponseHeaderStateAccess::knownValue(response, bit);
}

[[nodiscard]] inline std::pmr::memory_resource* responseResource(
    const HttpResponse& response) noexcept {
    return HttpResponseHeaderStateAccess::resource(response);
}

}  // namespace ruvia::detail
