#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "ruvia/http/detail/util/borrowed_view.h"
#include "ruvia/http/http2_types.h"

#include "http2/http2_frame_codec.h"

namespace ruvia::detail {

enum class http2_frame_payload_status : std::uint8_t {
    decoded,
    invalid_padding,
    missing_priority_fields,
};

// 调用方已经验证五字节字段完整；HEADERS 与 PRIORITY 共用线上解码。
[[nodiscard]] inline http2_received_priority http2_decode_priority_fields(
    std::uint32_t stream_id, const unsigned char* data) noexcept {
    const auto dependency = http2_read32(data);
    return {.stream_id_ = stream_id,
        .stream_dependency_ = dependency & 0x7fffffffU,
        .weight_ = static_cast<std::uint16_t>(static_cast<unsigned>(data[4]) + 1U),
        .exclusive_ = (dependency & 0x80000000U) != 0};
}

// Single owner of the DATA/HEADERS payload framing per RFC 9113 §6.1/§6.2:
// strip the optional Pad Length prefix and trailing padding, and (HEADERS only,
// when `allow_priority`) skip the 5-byte priority field. Keep malformed padding
// distinct from missing mandatory priority fields: both are connection errors for
// HEADERS, but RFC 9113 requires PROTOCOL_ERROR for the former and
// FRAME_SIZE_ERROR for the latter.
[[nodiscard]] inline http2_frame_payload_status http2_strip_pad_and_priority(
    const http2_frame_header& header_value, std::string_view payload_value, bool allow_priority,
    std::string_view& content, http2_received_priority* priority = nullptr) noexcept {
    std::size_t offset = 0;
    std::size_t padding = 0;
    if ((header_value.flags_ & http2_flag_padded) != 0) {
        if (payload_value.empty()) {
            return http2_frame_payload_status::invalid_padding;
        }
        padding = static_cast<unsigned char>(payload_value[0]);
        offset = 1;
    }
    if (allow_priority && (header_value.flags_ & http2_flag_priority) != 0) {
        if (payload_value.size() < offset + 5) {
            return http2_frame_payload_status::missing_priority_fields;
        }
        if (priority != nullptr) {
            *priority = http2_decode_priority_fields(header_value.stream_id_,
                reinterpret_cast<const unsigned char*>(payload_value.data() + offset));
        }
        offset += 5;
    }
    if (payload_value.size() < offset + padding) {
        return http2_frame_payload_status::invalid_padding;
    }
    content = payload_value.substr(offset, payload_value.size() - offset - padding);
    return http2_frame_payload_status::decoded;
}

template <http_temporary_owning_char_string payload>
http2_frame_payload_status http2_strip_pad_and_priority(
    const http2_frame_header&, payload&&, bool, std::string_view&, http2_received_priority* = nullptr) = delete;

[[nodiscard]] inline http2_frame_payload_status http2_decode_headers_payload(
    const http2_frame_header& header_value, std::string_view payload_value, std::string_view& fragment,
    http2_received_priority* priority = nullptr) noexcept {
    return http2_strip_pad_and_priority(header_value, payload_value, true, fragment, priority);
}

template <http_temporary_owning_char_string payload>
http2_frame_payload_status http2_decode_headers_payload(
    const http2_frame_header&, payload&&, std::string_view&, http2_received_priority* = nullptr) = delete;

[[nodiscard]] inline bool http2_decode_data_payload(
    const http2_frame_header& header_value, std::string_view payload_value, std::string_view& data) noexcept {
    return http2_strip_pad_and_priority(header_value, payload_value, false, data) ==
           http2_frame_payload_status::decoded;
}

template <http_temporary_owning_char_string payload>
bool http2_decode_data_payload(const http2_frame_header&, payload&&, std::string_view&) = delete;

}  // namespace ruvia::detail
