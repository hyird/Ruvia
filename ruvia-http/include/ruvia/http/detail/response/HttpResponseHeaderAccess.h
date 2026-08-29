#pragma once

#include "ruvia/http/HttpResponse.h"

#include <cstdint>
#include <limits>
#include <stdexcept>

namespace ruvia::detail {

// HttpResponseHeader stores both lengths in uint32_t and releases the one
// name/value allocation using their sum. Never let a public or runtime helper
// construct a descriptor whose wire/storage lengths cannot be represented;
// truncating either field would make the response header view disagree with
// the allocation and could turn a large application value into malformed wire
// bytes or an out-of-bounds read during emission.
[[nodiscard]] inline constexpr bool responseHeaderStorageSizeFits(
    std::size_t nameSize, std::size_t valueSize) noexcept {
    constexpr auto maxSize = static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)());
    return nameSize <= maxSize && valueSize <= maxSize && valueSize <= maxSize - nameSize;
}

inline void validateResponseHeaderStorageSize(std::size_t nameSize, std::size_t valueSize) {
    if (!responseHeaderStorageSizeFits(nameSize, valueSize)) {
        throw std::length_error("HTTP response header is too large");
    }
}

struct HttpResponseHeaderAccess final {
    [[nodiscard]] static HttpResponseHeader make(const char* bytes, std::uint32_t nameSize,
        std::uint32_t valueSize, std::uint32_t knownBit, bool owned) noexcept {
        HttpResponseHeader header;
        header.bytes = bytes;
        header.nameSize = nameSize;
        header.valueSize = valueSize;
        header.knownBit = knownBit;
        header.owned = owned;
        header.append = false;
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

    [[nodiscard]] static bool append(const HttpResponseHeader& header) noexcept {
        return header.append;
    }

    static void setAppend(HttpResponseHeader& header, bool value) noexcept {
        header.append = value;
    }
};

[[nodiscard]] inline HttpResponseHeader makeResponseHeader(const char* bytes,
    std::uint32_t nameSize, std::uint32_t valueSize, std::uint32_t knownBit, bool owned) noexcept {
    return HttpResponseHeaderAccess::make(bytes, nameSize, valueSize, knownBit, owned);
}

[[nodiscard]] inline char* responseHeaderValueBegin(HttpResponseHeader& header) noexcept {
    return HttpResponseHeaderAccess::valueBegin(header);
}

[[nodiscard]] inline char* responseHeaderValueEnd(HttpResponseHeader& header) noexcept {
    return HttpResponseHeaderAccess::valueEnd(header);
}

[[nodiscard]] inline std::uint32_t responseHeaderKnownBit(
    const HttpResponseHeader& header) noexcept {
    return HttpResponseHeaderAccess::knownBit(header);
}

[[nodiscard]] inline bool responseHeaderAppend(const HttpResponseHeader& header) noexcept {
    return HttpResponseHeaderAccess::append(header);
}

inline void setResponseHeaderAppend(HttpResponseHeader& header, bool value) noexcept {
    HttpResponseHeaderAccess::setAppend(header, value);
}

}  // namespace ruvia::detail
