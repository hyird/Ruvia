#pragma once

#include <cstdint>

#include "Http2Hpack.h"

namespace ruvia::detail {

enum class HeaderDecodeStatus : std::uint8_t {
    kOk,
    kProtocolError,
    kCompressionError
};

[[nodiscard]] inline HeaderDecodeStatus http2ClassifyHeaderDecodeResult(
    HpackDecodeResult result) noexcept {
    if (result.ok()) {
        return HeaderDecodeStatus::kOk;
    }
    return result.error == HpackError::kCallbackRejected
        ? HeaderDecodeStatus::kProtocolError
        : HeaderDecodeStatus::kCompressionError;
}

}  // namespace ruvia::detail
