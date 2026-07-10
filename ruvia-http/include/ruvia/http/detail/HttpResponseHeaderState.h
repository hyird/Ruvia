#pragma once

#include "ruvia/http/detail/HttpResponseHeaderBits.h"
#include "ruvia/http/detail/HttpResponseKnownHeaders.h"
#include "ruvia/http/HttpResponse.h"

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string_view>

namespace ruvia::detail {

struct HttpResponseHeaderStateAccess final {
    static void setStableView(
        HttpResponse& response,
        std::string_view key,
        std::string_view value) {
        response.setHeaderStableView(key, value);
    }

    static void setValidated(
        HttpResponse& response,
        std::string_view key,
        std::string_view value,
        std::uint32_t knownBit) {
        response.setHeaderValidated(key, value, knownBit);
    }

    static void appendValidated(
        HttpResponse& response,
        std::string_view key,
        std::string_view value,
        std::uint32_t knownBit) {
        response.appendHeaderValidated(key, value, knownBit);
    }

    static void setUnsigned(
        HttpResponse& response,
        std::string_view key,
        std::uint64_t value,
        std::uint32_t knownBit) {
        response.setHeaderUnsigned(key, value, knownBit);
    }

    static void setAllow(HttpResponse& response, std::uint32_t methodMask) {
        response.setAllowHeader(methodMask);
    }

    static void setContentRange(
        HttpResponse& response,
        std::uint64_t offset,
        std::uint64_t length,
        std::uint64_t size) {
        response.setContentRange(offset, length, size);
    }

    static void setContentRangeUnsatisfied(HttpResponse& response, std::uint64_t size) {
        response.setContentRangeUnsatisfied(size);
    }

    static void reserve(HttpResponse& response, std::size_t count) {
        response.reserveHeaders(count);
    }

    [[nodiscard]] static std::uint32_t classifyKnown(std::string_view name) noexcept {
        return classifyResponseHeaderName(name);
    }

    [[nodiscard]] static std::uint32_t knownBits(const HttpResponse& response) noexcept {
        return response.knownHeaderBits_;
    }

    [[nodiscard]] static bool hasKnown(const HttpResponse& response, std::uint32_t bit) noexcept {
        return (response.knownHeaderBits_ & bit) != 0;
    }

    [[nodiscard]] static std::string_view knownValue(
        const HttpResponse& response,
        std::uint32_t bit) noexcept {
        return response.knownHeaderValue(bit);
    }

    [[nodiscard]] static std::pmr::memory_resource* resource(const HttpResponse& response) noexcept {
        return response.resource();
    }
};

inline void setResponseHeaderStableView(
    HttpResponse& response,
    std::string_view key,
    std::string_view value) {
    HttpResponseHeaderStateAccess::setStableView(response, key, value);
}

inline void setResponseHeaderValidated(
    HttpResponse& response,
    std::string_view key,
    std::string_view value,
    std::uint32_t knownBit) {
    HttpResponseHeaderStateAccess::setValidated(response, key, value, knownBit);
}

inline void appendResponseHeaderValidated(
    HttpResponse& response,
    std::string_view key,
    std::string_view value,
    std::uint32_t knownBit) {
    HttpResponseHeaderStateAccess::appendValidated(response, key, value, knownBit);
}

inline void setResponseHeaderUnsigned(
    HttpResponse& response,
    std::string_view key,
    std::uint64_t value,
    std::uint32_t knownBit) {
    HttpResponseHeaderStateAccess::setUnsigned(response, key, value, knownBit);
}

inline void setResponseAllowHeader(HttpResponse& response, std::uint32_t methodMask) {
    HttpResponseHeaderStateAccess::setAllow(response, methodMask);
}

inline void setResponseContentRange(
    HttpResponse& response,
    std::uint64_t offset,
    std::uint64_t length,
    std::uint64_t size) {
    HttpResponseHeaderStateAccess::setContentRange(response, offset, length, size);
}

inline void setResponseContentRangeUnsatisfied(HttpResponse& response, std::uint64_t size) {
    HttpResponseHeaderStateAccess::setContentRangeUnsatisfied(response, size);
}

inline void reserveResponseHeaders(HttpResponse& response, std::size_t count) {
    HttpResponseHeaderStateAccess::reserve(response, count);
}

[[nodiscard]] inline std::uint32_t classifyResponseKnownHeader(std::string_view name) noexcept {
    return HttpResponseHeaderStateAccess::classifyKnown(name);
}

[[nodiscard]] inline std::uint32_t responseKnownHeaderBits(const HttpResponse& response) noexcept {
    return HttpResponseHeaderStateAccess::knownBits(response);
}

[[nodiscard]] inline bool responseHasKnownHeader(const HttpResponse& response, std::uint32_t bit) noexcept {
    return HttpResponseHeaderStateAccess::hasKnown(response, bit);
}

[[nodiscard]] inline std::string_view responseKnownHeader(
    const HttpResponse& response,
    std::uint32_t bit) noexcept {
    return HttpResponseHeaderStateAccess::knownValue(response, bit);
}

[[nodiscard]] inline std::pmr::memory_resource* responseResource(const HttpResponse& response) noexcept {
    return HttpResponseHeaderStateAccess::resource(response);
}

}  // namespace ruvia::detail
