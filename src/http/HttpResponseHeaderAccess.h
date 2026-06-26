#pragma once

#include "ruvia/http/HttpResponse.h"

#include <cstdint>

namespace ruvia::detail {

struct HttpResponseHeaderAccess final {
    [[nodiscard]] static HttpResponseHeader make(
        const char* bytes,
        std::uint32_t nameSize,
        std::uint32_t valueSize,
        std::uint32_t knownBit,
        bool owned) noexcept {
        HttpResponseHeader header;
        header.bytes = bytes;
        header.nameSize = nameSize;
        header.valueSize = valueSize;
        header.knownBit = knownBit;
        header.owned = owned;
        return header;
    }

    [[nodiscard]] static char* valueBegin(HttpResponseHeader& header) noexcept {
        if (header.bytes == nullptr) {
            return nullptr;
        }
        return const_cast<char*>(header.bytes) + header.nameSize;
    }

    [[nodiscard]] static char* valueEnd(HttpResponseHeader& header) noexcept {
        auto* const begin = valueBegin(header);
        return begin == nullptr ? nullptr : begin + header.valueSize;
    }

    [[nodiscard]] static std::uint32_t knownBit(const HttpResponseHeader& header) noexcept {
        return header.knownBit;
    }
};

[[nodiscard]] inline HttpResponseHeader makeResponseHeader(
    const char* bytes,
    std::uint32_t nameSize,
    std::uint32_t valueSize,
    std::uint32_t knownBit,
    bool owned) noexcept {
    return HttpResponseHeaderAccess::make(bytes, nameSize, valueSize, knownBit, owned);
}

[[nodiscard]] inline char* responseHeaderValueBegin(HttpResponseHeader& header) noexcept {
    return HttpResponseHeaderAccess::valueBegin(header);
}

[[nodiscard]] inline char* responseHeaderValueEnd(HttpResponseHeader& header) noexcept {
    return HttpResponseHeaderAccess::valueEnd(header);
}

[[nodiscard]] inline std::uint32_t responseHeaderKnownBit(const HttpResponseHeader& header) noexcept {
    return HttpResponseHeaderAccess::knownBit(header);
}

}  // namespace ruvia::detail
