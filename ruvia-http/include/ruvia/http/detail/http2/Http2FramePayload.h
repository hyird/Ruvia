#pragma once

#include "ruvia/http/detail/http2/Http2FrameCodec.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace ruvia::detail {

// Single owner of the DATA/HEADERS payload framing per RFC 9113 §6.1/§6.2:
// strip the optional Pad Length prefix and trailing padding, and (HEADERS only,
// when `allowPriority`) skip the 5-byte priority field, optionally returning its
// stream dependency. `content` is set to the surviving fragment. Returns false on
// malformed padding/priority sizing. The `allowPriority`/`dependency` arguments
// are compile-time-constant at every call site, so the branches fold away.
[[nodiscard]] inline bool http2StripPadAndPriority(
    const Http2FrameHeader& header,
    std::string_view payload,
    bool allowPriority,
    std::string_view& content,
    std::uint32_t* dependency = nullptr) noexcept {
    std::size_t offset = 0;
    std::size_t padding = 0;
    if ((header.flags & kHttp2FlagPadded) != 0) {
        if (payload.empty()) {
            return false;
        }
        padding = static_cast<unsigned char>(payload[0]);
        offset = 1;
    }
    if (allowPriority && (header.flags & kHttp2FlagPriority) != 0) {
        if (payload.size() < offset + 5) {
            return false;
        }
        if (dependency != nullptr) {
            *dependency = http2Read31(reinterpret_cast<const unsigned char*>(payload.data() + offset));
        }
        offset += 5;
    }
    if (payload.size() < offset + padding) {
        return false;
    }
    content = payload.substr(offset, payload.size() - offset - padding);
    return true;
}

[[nodiscard]] inline bool http2DecodeHeadersPayload(
    const Http2FrameHeader& header,
    std::string_view payload,
    std::string_view& fragment) noexcept {
    return http2StripPadAndPriority(header, payload, true, fragment);
}

[[nodiscard]] inline bool http2HeadersPriorityDependency(
    const Http2FrameHeader& header,
    std::string_view payload,
    std::uint32_t& dependency) noexcept {
    dependency = 0;
    std::string_view content;
    return http2StripPadAndPriority(header, payload, true, content, &dependency);
}

[[nodiscard]] inline bool http2DecodeDataPayload(
    const Http2FrameHeader& header,
    std::string_view payload,
    std::string_view& data) noexcept {
    return http2StripPadAndPriority(header, payload, false, data);
}

}  // namespace ruvia::detail
