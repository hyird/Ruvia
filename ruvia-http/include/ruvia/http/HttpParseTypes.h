#pragma once

#include <cstdint>
#include <string_view>

namespace ruvia {

enum class HttpParseStatus {
    kComplete,
    kIncomplete,
    kError
};

enum class HttpParseError {
    kNone,
    kHeaderTooLarge,
    kBodyTooLarge,
    kInvalidRequestLine,
    kUnsupportedHttpVersion,
    kInvalidRequestTarget,
    kUnsupportedMethod,
    kInvalidHeader,
    kTooManyHeaders,
    kMissingHost,
    kInvalidHost,
    kInvalidContentLength,
    kConflictingContentLength,
    kInvalidTransferEncoding,
    kUnsupportedTransferEncoding,
    kExpectationFailed,
    kInvalidChunkSize,
    kChunkSizeOverflow,
    kInvalidChunkExtension,
    kInvalidChunkCrlf,
    kInvalidTrailer
};

[[nodiscard]] std::string_view httpParseErrorMessage(HttpParseError error) noexcept;
[[nodiscard]] std::uint16_t httpParseErrorStatus(HttpParseError error) noexcept;

}  // namespace ruvia
