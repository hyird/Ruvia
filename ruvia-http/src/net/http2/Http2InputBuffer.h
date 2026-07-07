#pragma once

#include <cstddef>
#include <memory_resource>
#include <string>
#include <string_view>

#include "ruvia/http/HttpLimits.h"
#include "ruvia/http/detail/PmrString.h"

namespace ruvia::detail {

inline constexpr std::size_t kHttp2InputCompactThresholdBytes = 64 * 1024;

// Once the input buffer has fully drained, release capacity that grew past this
// during a large-frame burst, so a long-lived connection does not pin peak input
// memory for its whole lifetime. Tied to the advertised max header list size
// (SETTINGS_MAX_HEADER_LIST_SIZE = kMaxHttpHeaderBytes), the largest header-block
// input a peer may send; mirrors HTTP/1's kReadBufferShrinkCapacityBytes.
inline constexpr std::size_t kHttp2InputRetainedBytes = kMaxHttpHeaderBytes;

[[nodiscard]] inline std::size_t http2AvailableInput(
    const std::pmr::string& input,
    std::size_t offset) noexcept {
    return input.size() - offset;
}

[[nodiscard]] inline std::string_view http2InputView(
    const std::pmr::string& input,
    std::size_t offset,
    std::size_t size) noexcept {
    return std::string_view(input.data() + offset, size);
}

inline void http2ConsumeInput(
    std::pmr::string& input,
    std::size_t& offset,
    std::size_t size) {
    offset += size;
    compactConsumedPrefix(input, offset, kHttp2InputCompactThresholdBytes);
}

// Reclaim an oversized input buffer once it is fully consumed. Only safe to call
// when no view returned by http2InputView/readFrame is still in use: the frame
// loop calls it right after consuming a frame, where the just-read payload is
// already dead (consumeInput may already reallocate, so nothing may hold a view
// across it). A buffer still holding pipelined bytes is left untouched.
inline void http2ReclaimDrainedInput(std::pmr::string& input) {
    if (input.empty()) {
        clearPmrStringRetainingSmall(input, kHttp2InputRetainedBytes);
    }
}

}  // namespace ruvia::detail
