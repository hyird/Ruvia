#pragma once

#include "ruvia/http/detail/util/BorrowedView.h"
#include "ruvia/http/detail/http2/frame/Http2FrameCodec.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace ruvia::detail {

enum class Http2FramePayloadStatus : std::uint8_t {
    kDecoded,
    kInvalidPadding,
    kMissingPriorityFields,
};

// Single owner of the DATA/HEADERS payload framing per RFC 9113 §6.1/§6.2:
// strip the optional Pad Length prefix and trailing padding, and (HEADERS only,
// when `allowPriority`) skip the 5-byte priority field, optionally returning its
// stream dependency. Keep malformed padding distinct from missing mandatory
// priority fields: both are connection errors for HEADERS, but RFC 9113 requires
// PROTOCOL_ERROR for the former and FRAME_SIZE_ERROR for the latter.
[[nodiscard]] inline Http2FramePayloadStatus http2StripPadAndPriority(const Http2FrameHeader& header, std::string_view payload, bool allowPriority, std::string_view& content, std::uint32_t* dependency = nullptr) noexcept {
    std::size_t offset = 0;
    std::size_t padding = 0;
    if ((header.flags & kHttp2FlagPadded) != 0) {
        if (payload.empty()) {
            return Http2FramePayloadStatus::kInvalidPadding;
        }
        padding = static_cast<unsigned char>(payload[0]);
        offset = 1;
    }
    if (allowPriority && (header.flags & kHttp2FlagPriority) != 0) {
        if (payload.size() < offset + 5) {
            return Http2FramePayloadStatus::kMissingPriorityFields;
        }
        if (dependency != nullptr) {
            *dependency = http2Read31(reinterpret_cast<const unsigned char*>(payload.data() + offset));
        }
        offset += 5;
    }
    if (payload.size() < offset + padding) {
        return Http2FramePayloadStatus::kInvalidPadding;
    }
    content = payload.substr(offset, payload.size() - offset - padding);
    return Http2FramePayloadStatus::kDecoded;
}

template <HttpTemporaryOwningCharString Payload>
Http2FramePayloadStatus http2StripPadAndPriority(const Http2FrameHeader&, Payload&&, bool, std::string_view&, std::uint32_t* = nullptr) = delete;

[[nodiscard]] inline Http2FramePayloadStatus http2DecodeHeadersPayload(const Http2FrameHeader& header, std::string_view payload, std::string_view& fragment) noexcept {
    return http2StripPadAndPriority(header, payload, true, fragment);
}

template <HttpTemporaryOwningCharString Payload>
Http2FramePayloadStatus http2DecodeHeadersPayload(const Http2FrameHeader&, Payload&&, std::string_view&) = delete;

[[nodiscard]] inline Http2FramePayloadStatus http2HeadersPriorityDependency(const Http2FrameHeader& header, std::string_view payload, std::uint32_t& dependency) noexcept {
    dependency = 0;
    std::string_view content;
    return http2StripPadAndPriority(header, payload, true, content, &dependency);
}

[[nodiscard]] inline bool http2DecodeDataPayload(const Http2FrameHeader& header, std::string_view payload, std::string_view& data) noexcept {
    return http2StripPadAndPriority(header, payload, false, data) == Http2FramePayloadStatus::kDecoded;
}

template <HttpTemporaryOwningCharString Payload>
bool http2DecodeDataPayload(const Http2FrameHeader&, Payload&&, std::string_view&) = delete;

}  // namespace ruvia::detail
