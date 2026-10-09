#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "ruvia/http/detail/util/borrowed_view.h"

#include "http2/http2_frame_codec.h"

namespace ruvia::detail {

enum class http2_frame_payload_status : std::uint8_t {
    decoded,
    invalid_padding,
    missing_priority_fields,
};

// Single owner of the DATA/HEADERS payload framing per RFC 9113 §6.1/§6.2:
// strip the optional Pad Length prefix and trailing padding, and (HEADERS only,
// when `allow_priority`) skip the 5-byte priority field, optionally returning its
// stream dependency. Keep malformed padding distinct from missing mandatory
// priority fields: both are connection errors for HEADERS, but RFC 9113 requires
// PROTOCOL_ERROR for the former and FRAME_SIZE_ERROR for the latter.
[[nodiscard]] inline http2_frame_payload_status http2_strip_pad_and_priority(
    const http2_frame_header& header_value, std::string_view payload_value, bool allow_priority,
    std::string_view& content, std::uint32_t* dependency = nullptr) noexcept {
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
        if (dependency != nullptr) {
            *dependency =
                http2_read31(reinterpret_cast<const unsigned char*>(payload_value.data() + offset));
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
    const http2_frame_header&, payload&&, bool, std::string_view&, std::uint32_t* = nullptr) = delete;

[[nodiscard]] inline http2_frame_payload_status http2_decode_headers_payload(
    const http2_frame_header& header_value, std::string_view payload_value, std::string_view& fragment) noexcept {
    return http2_strip_pad_and_priority(header_value, payload_value, true, fragment);
}

template <http_temporary_owning_char_string payload>
http2_frame_payload_status http2_decode_headers_payload(
    const http2_frame_header&, payload&&, std::string_view&) = delete;

[[nodiscard]] inline http2_frame_payload_status http2_headers_priority_dependency(
    const http2_frame_header& header_value, std::string_view payload_value, std::uint32_t& dependency) noexcept {
    dependency = 0;
    std::string_view content;
    return http2_strip_pad_and_priority(header_value, payload_value, true, content, &dependency);
}

[[nodiscard]] inline bool http2_decode_data_payload(
    const http2_frame_header& header_value, std::string_view payload_value, std::string_view& data) noexcept {
    return http2_strip_pad_and_priority(header_value, payload_value, false, data) ==
           http2_frame_payload_status::decoded;
}

template <http_temporary_owning_char_string payload>
bool http2_decode_data_payload(const http2_frame_header&, payload&&, std::string_view&) = delete;

}  // namespace ruvia::detail
