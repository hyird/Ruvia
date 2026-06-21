#pragma once

#include "Http2FrameTypes.h"

#include <memory_resource>
#include <string>
#include <string_view>

namespace ruvia::detail {

[[nodiscard]] inline std::uint16_t http2Read16(const unsigned char* data) noexcept {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[0]) << 8) | data[1]);
}

[[nodiscard]] inline std::uint32_t http2Read24(const unsigned char* data) noexcept {
    return (static_cast<std::uint32_t>(data[0]) << 16) |
        (static_cast<std::uint32_t>(data[1]) << 8) |
        static_cast<std::uint32_t>(data[2]);
}

[[nodiscard]] inline std::uint32_t http2Read31(const unsigned char* data) noexcept {
    return ((static_cast<std::uint32_t>(data[0] & 0x7f) << 24) |
            (static_cast<std::uint32_t>(data[1]) << 16) |
            (static_cast<std::uint32_t>(data[2]) << 8) |
            static_cast<std::uint32_t>(data[3]));
}

[[nodiscard]] inline std::uint32_t http2Read32(const unsigned char* data) noexcept {
    return (static_cast<std::uint32_t>(data[0]) << 24) |
        (static_cast<std::uint32_t>(data[1]) << 16) |
        (static_cast<std::uint32_t>(data[2]) << 8) |
        static_cast<std::uint32_t>(data[3]);
}

inline void http2Append16(std::pmr::string& out, std::uint16_t value) {
    out.push_back(static_cast<char>((value >> 8) & 0xff));
    out.push_back(static_cast<char>(value & 0xff));
}

inline void http2Append24(std::pmr::string& out, std::uint32_t value) {
    out.push_back(static_cast<char>((value >> 16) & 0xff));
    out.push_back(static_cast<char>((value >> 8) & 0xff));
    out.push_back(static_cast<char>(value & 0xff));
}

inline void http2Append32(std::pmr::string& out, std::uint32_t value) {
    out.push_back(static_cast<char>((value >> 24) & 0xff));
    out.push_back(static_cast<char>((value >> 16) & 0xff));
    out.push_back(static_cast<char>((value >> 8) & 0xff));
    out.push_back(static_cast<char>(value & 0xff));
}

inline void http2EncodeFrameHeader(
    char* out,
    std::uint32_t length,
    Http2FrameType type,
    std::uint8_t flags,
    std::uint32_t streamId) noexcept {
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

[[nodiscard]] inline Http2FrameHeader http2ParseFrameHeader(std::string_view bytes) noexcept {
    const auto* data = reinterpret_cast<const unsigned char*>(bytes.data());
    return Http2FrameHeader{
        .length = http2Read24(data),
        .type = data[3],
        .flags = data[4],
        .streamId = http2Read31(data + 5)};
}

inline void http2AppendFrameHeader(
    std::pmr::string& out,
    std::uint32_t length,
    Http2FrameType type,
    std::uint8_t flags,
    std::uint32_t streamId) {
    char header[kHttp2FrameHeaderBytes];
    http2EncodeFrameHeader(header, length, type, flags, streamId);
    out.append(header, kHttp2FrameHeaderBytes);
}

inline void http2AppendFrame(
    std::pmr::string& out,
    Http2FrameType type,
    std::uint8_t flags,
    std::uint32_t streamId,
    std::string_view payload) {
    http2AppendFrameHeader(out, static_cast<std::uint32_t>(payload.size()), type, flags, streamId);
    out.append(payload.data(), payload.size());
}

inline void http2AppendSettingsEntry(std::pmr::string& out, Http2SettingId id, std::uint32_t value) {
    http2Append16(out, static_cast<std::uint16_t>(id));
    http2Append32(out, value);
}

inline void http2AppendRstStream(std::pmr::string& out, std::uint32_t streamId, Http2ErrorCode error) {
    http2AppendFrameHeader(out, 4, Http2FrameType::kRstStream, 0, streamId);
    http2Append32(out, static_cast<std::uint32_t>(error));
}

inline void http2AppendWindowUpdate(std::pmr::string& out, std::uint32_t streamId, std::uint32_t increment) {
    http2AppendFrameHeader(out, 4, Http2FrameType::kWindowUpdate, 0, streamId);
    http2Append32(out, increment & 0x7fffffffU);
}

inline void http2AppendGoaway(
    std::pmr::string& out,
    std::uint32_t lastStreamId,
    Http2ErrorCode error,
    std::string_view debug = {}) {
    http2AppendFrameHeader(out, static_cast<std::uint32_t>(8 + debug.size()), Http2FrameType::kGoaway, 0, 0);
    http2Append32(out, lastStreamId & 0x7fffffffU);
    http2Append32(out, static_cast<std::uint32_t>(error));
    out.append(debug.data(), debug.size());
}

}  // namespace ruvia::detail
