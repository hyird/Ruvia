#pragma once

#include "ruvia/http/HttpProtocolError.h"

#include <cstdint>

namespace ruvia {

enum class HttpParseError : std::uint8_t {
    kHeaderTooLarge,
    kBodyTooLarge,
    kInvalidRequestLine,
    kUnsupportedHttpVersion,
    kInvalidRequestTarget,
    kInvalidHeader,
    kInvalidConnection,
    kInvalidUpgrade,
    kTooManyHeaders,
    kMissingHost,
    kInvalidHost,
    kInvalidContentLength,
    kConflictingContentLength,
    kInvalidTransferEncoding,
    kUnsupportedTransferEncoding,
    kInvalidChunkSize,
    kChunkSizeOverflow,
    kInvalidChunkExtension,
    kInvalidChunkCrlf,
    kInvalidTrailer
};

[[nodiscard]] HttpProtocolError httpParseProtocolError(HttpParseError error) noexcept;

}  // namespace ruvia
