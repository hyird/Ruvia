#pragma once

#include <cstddef>
#include <string_view>

namespace ruvia::detail {

enum class HttpChunkScanStatus {
    kComplete,
    kIncomplete,
    kInvalidSize,
    kSizeOverflow,
    kInvalidExtension,
    kInvalidCrlf,
    kInvalidTrailer,
    kTooLarge
};

struct HttpChunkScanResult {
    HttpChunkScanStatus status{HttpChunkScanStatus::kIncomplete};
    std::size_t consumedBytes{0};
};

[[nodiscard]] bool parseHttpChunkSize(std::string_view value, std::size_t& size) noexcept;
[[nodiscard]] HttpChunkScanStatus validateHttpChunkTrailers(std::string_view trailers) noexcept;
[[nodiscard]] HttpChunkScanResult scanHttpChunkedBody(std::string_view body) noexcept;

}  // namespace ruvia::detail
