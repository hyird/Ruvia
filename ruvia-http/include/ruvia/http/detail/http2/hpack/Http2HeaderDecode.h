#pragma once

#include <cstdint>

#include "ruvia/http/HpackProtocolTypes.h"

namespace ruvia::detail {

enum class HeaderDecodeStatus : std::uint8_t { kOk,
    kProtocolError,
    kCompressionError };

[[nodiscard]] inline HeaderDecodeStatus http2ClassifyHeaderDecodeResult(
    const HpackDecodeResult& result) noexcept {
    if (result.decoded()) {
        return HeaderDecodeStatus::kOk;
    }
    return result.error() == HpackDecodeError::kCallbackRejected
               ? HeaderDecodeStatus::kProtocolError
               : HeaderDecodeStatus::kCompressionError;
}

}  // namespace ruvia::detail
