#pragma once

#include "Http2FrameCodec.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace ruvia::detail {

[[nodiscard]] inline bool http2DecodeHeadersPayload(
    const Http2FrameHeader& header,
    std::string_view payload,
    std::string_view& fragment) noexcept {
    std::size_t offset = 0;
    std::size_t padding = 0;
    if ((header.flags & kHttp2FlagPadded) != 0) {
        if (payload.empty()) {
            return false;
        }
        padding = static_cast<unsigned char>(payload[0]);
        offset = 1;
    }
    if ((header.flags & kHttp2FlagPriority) != 0) {
        if (payload.size() < offset + 5) {
            return false;
        }
        offset += 5;
    }
    if (payload.size() < offset + padding) {
        return false;
    }
    fragment = payload.substr(offset, payload.size() - offset - padding);
    return true;
}

[[nodiscard]] inline bool http2HeadersPriorityDependency(
    const Http2FrameHeader& header,
    std::string_view payload,
    std::uint32_t& dependency) noexcept {
    dependency = 0;
    std::size_t offset = 0;
    std::size_t padding = 0;
    if ((header.flags & kHttp2FlagPadded) != 0) {
        if (payload.empty()) {
            return false;
        }
        padding = static_cast<unsigned char>(payload[0]);
        offset = 1;
    }
    if ((header.flags & kHttp2FlagPriority) != 0) {
        if (payload.size() < offset + 5) {
            return false;
        }
        dependency = http2Read31(reinterpret_cast<const unsigned char*>(payload.data() + offset));
        offset += 5;
    }
    return payload.size() >= offset + padding;
}

[[nodiscard]] inline bool http2DecodeDataPayload(
    const Http2FrameHeader& header,
    std::string_view payload,
    std::string_view& data) noexcept {
    std::size_t offset = 0;
    std::size_t padding = 0;
    if ((header.flags & kHttp2FlagPadded) != 0) {
        if (payload.empty()) {
            return false;
        }
        padding = static_cast<unsigned char>(payload[0]);
        offset = 1;
    }
    if (payload.size() < offset + padding) {
        return false;
    }
    data = payload.substr(offset, payload.size() - offset - padding);
    return true;
}

}  // namespace ruvia::detail
