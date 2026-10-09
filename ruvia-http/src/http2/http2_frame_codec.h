#pragma once

#include <span>
#include <string_view>
#include <utility>

#include "http2/http2_frame_types.h"

namespace ruvia::detail {

[[nodiscard]] inline std::uint16_t http2_read16(const unsigned char* data) noexcept {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[0]) << 8) | data[1]);
}

[[nodiscard]] inline std::uint32_t http2_read24(const unsigned char* data) noexcept {
    return (static_cast<std::uint32_t>(data[0]) << 16) |
           (static_cast<std::uint32_t>(data[1]) << 8) | static_cast<std::uint32_t>(data[2]);
}

[[nodiscard]] inline std::uint32_t http2_read31(const unsigned char* data) noexcept {
    return ((static_cast<std::uint32_t>(data[0] & 0x7f) << 24) |
            (static_cast<std::uint32_t>(data[1]) << 16) |
            (static_cast<std::uint32_t>(data[2]) << 8) | static_cast<std::uint32_t>(data[3]));
}

[[nodiscard]] inline std::uint32_t http2_read32(const unsigned char* data) noexcept {
    return (static_cast<std::uint32_t>(data[0]) << 24) |
           (static_cast<std::uint32_t>(data[1]) << 16) |
           (static_cast<std::uint32_t>(data[2]) << 8) | static_cast<std::uint32_t>(data[3]);
}

inline char* http2_write16(char* out, std::uint16_t value) noexcept {
    *out++ = static_cast<char>((value >> 8) & 0xff);
    *out++ = static_cast<char>(value & 0xff);
    return out;
}

inline char* http2_write32(char* out, std::uint32_t value) noexcept {
    *out++ = static_cast<char>((value >> 24) & 0xff);
    *out++ = static_cast<char>((value >> 16) & 0xff);
    *out++ = static_cast<char>((value >> 8) & 0xff);
    *out++ = static_cast<char>(value & 0xff);
    return out;
}

inline void http2_encode_frame_header(char* out, std::uint32_t length, http2_frame_type type,
    std::uint8_t flags, std::uint32_t stream_id) noexcept {
    write_http2_frame_header(std::span<char, http2_frame_header_bytes>(out, http2_frame_header_bytes),
        length, type, flags, stream_id);
}

inline char* http2_write_frame_header(char* out, std::uint32_t length, http2_frame_type type,
    std::uint8_t flags, std::uint32_t stream_id) noexcept {
    http2_encode_frame_header(out, length, type, flags, stream_id);
    return out + http2_frame_header_bytes;
}

[[nodiscard]] inline http2_frame_header http2_parse_frame_header(std::string_view bytes_value) noexcept {
    return decode_http2_frame_header(
        std::span<const char, http2_frame_header_bytes>(bytes_value.data(), http2_frame_header_bytes));
}

inline char* http2_write_settings_entry(char* out, http2_setting_id id, std::uint32_t value) noexcept {
    out = http2_write16(out, static_cast<std::uint16_t>(id));
    return http2_write32(out, value);
}

inline char* http2_write_window_update(
    char* out, std::uint32_t stream_id, std::uint32_t increment) noexcept {
    out = http2_write_frame_header(out, 4, http2_frame_type::window_update, 0, stream_id);
    return http2_write32(out, increment & 0x7fffffffU);
}

inline char* http2_write_goaway_payload(
    char* out, std::uint32_t last_stream_id, http2_error_code error) noexcept {
    out = http2_write32(out, last_stream_id & 0x7fffffffU);
    return http2_write32(out, static_cast<std::uint32_t>(error));
}

}  // namespace ruvia::detail
