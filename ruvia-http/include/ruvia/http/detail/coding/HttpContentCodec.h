#pragma once

#include <cstddef>
#include <memory_resource>
#include <string>
#include <string_view>
#include <variant>

#include "ruvia/http/HttpContentCodec.h"

// One compression library binding per coding, behind a uniform signature: decode
// or encode a whole buffer through the caller's memory resource, bounded by an
// explicit output ceiling, reporting failure as a value. Which coding a field
// asks for is decided elsewhere; this is only the machinery each one runs on.

namespace ruvia::detail {

using ContentEncodeAttempt = std::variant<std::pmr::string, HttpContentEncodeError>;
using ContentDecodeAttempt = std::variant<std::pmr::string, HttpContentDecodeError>;

// Append decoder output while enforcing the ceiling; false means the ceiling
// would be exceeded and the decode must fail.
[[nodiscard]] inline bool appendDecodedBytes(std::pmr::string& output, const char* bytes, std::size_t size, std::size_t maxDecodedBytes) {
    if (output.size() > maxDecodedBytes || size > maxDecodedBytes - output.size()) {
        return false;
    }
    output.append(bytes, size);
    return true;
}

[[nodiscard]] ContentDecodeAttempt decodeGzipContent(std::string_view input, std::size_t maxDecodedBytes, std::pmr::memory_resource* resource);
[[nodiscard]] ContentDecodeAttempt decodeBrotliContent(std::string_view input, std::size_t maxDecodedBytes, std::pmr::memory_resource* resource);
[[nodiscard]] ContentDecodeAttempt decodeZstdContent(std::string_view input, std::size_t maxDecodedBytes, std::pmr::memory_resource* resource);

[[nodiscard]] ContentEncodeAttempt encodeGzipContent(std::string_view input, std::size_t maxEncodedBytes, std::pmr::memory_resource* resource);
[[nodiscard]] ContentEncodeAttempt encodeBrotliContent(std::string_view input, std::size_t maxEncodedBytes, std::pmr::memory_resource* resource);
[[nodiscard]] ContentEncodeAttempt encodeZstdContent(std::string_view input, std::size_t maxEncodedBytes, std::pmr::memory_resource* resource);

}  // namespace ruvia::detail
