#pragma once

#include "ruvia/http/detail/http2/frame/Http2FrameTypes.h"

#include <string_view>
#include <utility>

namespace ruvia::detail {

[[nodiscard]] inline std::uint16_t http2Read16(const unsigned char* data) noexcept {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[0]) << 8) | data[1]);
}

[[nodiscard]] inline std::uint32_t http2Read24(const unsigned char* data) noexcept {
    return (static_cast<std::uint32_t>(data[0]) << 16) | (static_cast<std::uint32_t>(data[1]) << 8) | static_cast<std::uint32_t>(data[2]);
}

[[nodiscard]] inline std::uint32_t http2Read31(const unsigned char* data) noexcept {
    return ((static_cast<std::uint32_t>(data[0] & 0x7f) << 24) | (static_cast<std::uint32_t>(data[1]) << 16) | (static_cast<std::uint32_t>(data[2]) << 8) | static_cast<std::uint32_t>(data[3]));
}

[[nodiscard]] inline std::uint32_t http2Read32(const unsigned char* data) noexcept {
    return (static_cast<std::uint32_t>(data[0]) << 24) | (static_cast<std::uint32_t>(data[1]) << 16) | (static_cast<std::uint32_t>(data[2]) << 8) | static_cast<std::uint32_t>(data[3]);
}

inline char* http2Write16(char* out, std::uint16_t value) noexcept {
    *out++ = static_cast<char>((value >> 8) & 0xff);
    *out++ = static_cast<char>(value & 0xff);
    return out;
}

inline char* http2Write32(char* out, std::uint32_t value) noexcept {
    *out++ = static_cast<char>((value >> 24) & 0xff);
    *out++ = static_cast<char>((value >> 16) & 0xff);
    *out++ = static_cast<char>((value >> 8) & 0xff);
    *out++ = static_cast<char>(value & 0xff);
    return out;
}

inline void http2EncodeFrameHeader(char* out, std::uint32_t length, Http2FrameType type, std::uint8_t flags, std::uint32_t streamId) noexcept {
    out[0] = static_cast<char>((length >> 16) & 0xff);
    out[1] = static_cast<char>((length >> 8) & 0xff);
    out[2] = static_cast<char>(length & 0xff);
    out[3] = static_cast<char>(type);
    out[4] = static_cast<char>(flags);
    const auto id = streamId & 0x7fffffffU;
    out[5] = static_cast<char>((id >> 24) & 0xff);
    out[6] = static_cast<char>((id >> 16) & 0xff);
    out[7] = static_cast<char>((id >> 8) & 0xff);
    out[8] = static_cast<char>(id & 0xff);
}

inline char* http2WriteFrameHeader(char* out, std::uint32_t length, Http2FrameType type, std::uint8_t flags, std::uint32_t streamId) noexcept {
    http2EncodeFrameHeader(out, length, type, flags, streamId);
    return out + kHttp2FrameHeaderBytes;
}

[[nodiscard]] inline Http2FrameHeader http2ParseFrameHeader(std::string_view bytes) noexcept {
    const auto* data = reinterpret_cast<const unsigned char*>(bytes.data());
    return Http2FrameHeader{.length = http2Read24(data), .type = data[3], .flags = data[4], .streamId = http2Read31(data + 5)};
}

inline char* http2WriteSettingsEntry(char* out, Http2SettingId id, std::uint32_t value) noexcept {
    out = http2Write16(out, static_cast<std::uint16_t>(id));
    return http2Write32(out, value);
}

inline char* http2WriteWindowUpdate(char* out, std::uint32_t streamId, std::uint32_t increment) noexcept {
    out = http2WriteFrameHeader(out, 4, Http2FrameType::kWindowUpdate, 0, streamId);
    return http2Write32(out, increment & 0x7fffffffU);
}

inline char* http2WriteGoawayPayload(char* out, std::uint32_t lastStreamId, Http2ErrorCode error) noexcept {
    out = http2Write32(out, lastStreamId & 0x7fffffffU);
    return http2Write32(out, static_cast<std::uint32_t>(error));
}

}  // namespace ruvia::detail
