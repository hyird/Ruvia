#pragma once

#include <span>
#include <string_view>
#include <utility>

#include "ruvia/http/detail/http2/frame/Http2FrameTypes.h"

namespace ruvia::detail {

[[nodiscard]] inline std::uint16_t http2Read16(const unsigned char* data) noexcept {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[0]) << 8) | data[1]);
}

[[nodiscard]] inline std::uint32_t http2Read24(const unsigned char* data) noexcept {
    return (static_cast<std::uint32_t>(data[0]) << 16) |
           (static_cast<std::uint32_t>(data[1]) << 8) | static_cast<std::uint32_t>(data[2]);
}

[[nodiscard]] inline std::uint32_t http2Read31(const unsigned char* data) noexcept {
    return ((static_cast<std::uint32_t>(data[0] & 0x7f) << 24) |
            (static_cast<std::uint32_t>(data[1]) << 16) |
            (static_cast<std::uint32_t>(data[2]) << 8) | static_cast<std::uint32_t>(data[3]));
}

[[nodiscard]] inline std::uint32_t http2Read32(const unsigned char* data) noexcept {
    return (static_cast<std::uint32_t>(data[0]) << 24) |
           (static_cast<std::uint32_t>(data[1]) << 16) |
           (static_cast<std::uint32_t>(data[2]) << 8) | static_cast<std::uint32_t>(data[3]);
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

inline void http2EncodeFrameHeader(char* out, std::uint32_t length, Http2FrameType type,
    std::uint8_t flags, std::uint32_t streamId) noexcept {
    writeHttp2FrameHeader(std::span<char, kHttp2FrameHeaderBytes>(out, kHttp2FrameHeaderBytes),
        length, type, flags, streamId);
}

inline char* http2WriteFrameHeader(char* out, std::uint32_t length, Http2FrameType type,
    std::uint8_t flags, std::uint32_t streamId) noexcept {
    http2EncodeFrameHeader(out, length, type, flags, streamId);
    return out + kHttp2FrameHeaderBytes;
}

[[nodiscard]] inline Http2FrameHeader http2ParseFrameHeader(std::string_view bytes) noexcept {
    return decodeHttp2FrameHeader(
        std::span<const char, kHttp2FrameHeaderBytes>(bytes.data(), kHttp2FrameHeaderBytes));
}

inline char* http2WriteSettingsEntry(char* out, Http2SettingId id, std::uint32_t value) noexcept {
    out = http2Write16(out, static_cast<std::uint16_t>(id));
    return http2Write32(out, value);
}

inline char* http2WriteWindowUpdate(
    char* out, std::uint32_t streamId, std::uint32_t increment) noexcept {
    out = http2WriteFrameHeader(out, 4, Http2FrameType::kWindowUpdate, 0, streamId);
    return http2Write32(out, increment & 0x7fffffffU);
}

inline char* http2WriteGoawayPayload(
    char* out, std::uint32_t lastStreamId, Http2ErrorCode error) noexcept {
    out = http2Write32(out, lastStreamId & 0x7fffffffU);
    return http2Write32(out, static_cast<std::uint32_t>(error));
}

}  // namespace ruvia::detail
