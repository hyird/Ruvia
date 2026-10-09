#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace ruvia {

inline constexpr std::string_view http2_client_preface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
inline constexpr std::size_t http2_frame_header_bytes = 9;
inline constexpr std::uint32_t http2_default_max_frame_size = 16 * 1024;
inline constexpr std::uint32_t http2_max_frame_size = 16 * 1024 * 1024 - 1;

enum class http2_frame_type : std::uint8_t {
    data = 0x0,
    headers = 0x1,
    priority = 0x2,
    rst_stream = 0x3,
    settings = 0x4,
    push_promise = 0x5,
    ping = 0x6,
    goaway = 0x7,
    window_update = 0x8,
    continuation = 0x9,
    alternative_service = 0xa,
    origin = 0xc,
    priority_update = 0x10
};
enum class http2_error_code : std::uint32_t {
    no_error = 0x0,
    protocol_error = 0x1,
    internal_error = 0x2,
    flow_control_error = 0x3,
    settings_timeout = 0x4,
    stream_closed = 0x5,
    frame_size_error = 0x6,
    refused_stream = 0x7,
    cancel = 0x8,
    compression_error = 0x9,
    connect_error = 0xa,
    enhance_your_calm = 0xb,
    inadequate_security = 0xc,
    http11_required = 0xd
};
enum class http2_setting_id : std::uint16_t {
    header_table_size = 0x1,
    enable_push = 0x2,
    max_concurrent_streams = 0x3,
    initial_window_size = 0x4,
    max_frame_size = 0x5,
    max_header_list_size = 0x6,
    enable_connect_protocol = 0x8,
    no_rfc7540_priorities = 0x9
};

struct http2_frame_header final {
    std::uint32_t length_{0};
    std::uint8_t type_{0};
    std::uint8_t flags_{0};
    std::uint32_t stream_id_{0};
};

namespace detail {

[[nodiscard]] inline constexpr http2_frame_header decode_http2_frame_header(
    std::span<const char, http2_frame_header_bytes> bytes_value) noexcept {
    const auto byte = [bytes_value](std::size_t index) constexpr noexcept {
        return static_cast<std::uint32_t>(static_cast<unsigned char>(bytes_value[index]));
    };
    return http2_frame_header{
        .length_ = (byte(0) << 16) | (byte(1) << 8) | byte(2),
        .type_ = static_cast<std::uint8_t>(bytes_value[3]),
        .flags_ = static_cast<std::uint8_t>(bytes_value[4]),
        .stream_id_ = ((byte(5) & 0x7fU) << 24) | (byte(6) << 16) | (byte(7) << 8) | byte(8),
    };
}

inline constexpr void write_http2_frame_header(std::span<char, http2_frame_header_bytes> output,
    std::uint32_t length, http2_frame_type type, std::uint8_t flags, std::uint32_t stream_id) noexcept {
    output[0] = static_cast<char>((length >> 16) & 0xff);
    output[1] = static_cast<char>((length >> 8) & 0xff);
    output[2] = static_cast<char>(length & 0xff);
    output[3] = static_cast<char>(type);
    output[4] = static_cast<char>(flags);
    output[5] = static_cast<char>((stream_id >> 24) & 0x7f);
    output[6] = static_cast<char>((stream_id >> 16) & 0xff);
    output[7] = static_cast<char>((stream_id >> 8) & 0xff);
    output[8] = static_cast<char>(stream_id & 0xff);
}

}  // namespace detail

[[nodiscard]] inline constexpr std::optional<http2_frame_header> parse_http2_frame_header(
    std::span<const char> bytes_value) noexcept {
    if (bytes_value.size() < http2_frame_header_bytes) {
        return std::nullopt;
    }
    return detail::decode_http2_frame_header(bytes_value.first<http2_frame_header_bytes>());
}

[[nodiscard]] inline constexpr bool encode_http2_frame_header(std::span<char> output, std::uint32_t length,
    http2_frame_type type, std::uint8_t flags, std::uint32_t stream_id) noexcept {
    if (output.size() < http2_frame_header_bytes || length > http2_max_frame_size ||
        stream_id > 0x7fffffffU) {
        return false;
    }
    detail::write_http2_frame_header(output.first<http2_frame_header_bytes>(), length, type, flags, stream_id);
    return true;
}

}  // namespace ruvia
