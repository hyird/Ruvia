#pragma once

#include <cstdint>

#include "ruvia/http/detail/http2/hpack/Http2Hpack.h"

namespace ruvia::detail {

enum class HeaderDecodeStatus : std::uint8_t {
    kOk,
    kProtocolError,
    kCompressionError
};

[[nodiscard]] inline HeaderDecodeStatus http2ClassifyHeaderDecodeResult(
    const HpackDecodeResult& result) noexcept {
    if (result.decoded() != nullptr) {
        return HeaderDecodeStatus::kOk;
    }
    return result.failure()->error() == HpackDecodeError::kCallbackRejected
        ? HeaderDecodeStatus::kProtocolError
        : HeaderDecodeStatus::kCompressionError;
}

}  // namespace ruvia::detail
