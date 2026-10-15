#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/http/http3_field_section.h"
#include "ruvia/http/http3_frames.h"
#include "ruvia/http/http3_headers_frame.h"
#include "ruvia/http/http3_qpack_connection.h"
#include "ruvia/http/http3_var_int.h"

namespace ruvia::detail {

// Static-table/literal field section after reserved_prefix_bytes zero bytes.
[[nodiscard]] std::variant<std::pmr::vector<char>, http3_field_section_error> encode_static_http3_field_section(
    std::span<const http3_field_section_field_view> fields_value, std::pmr::memory_resource* resource,
    std::size_t reserved_prefix_bytes);

struct http3_headers_frame_access final {
    // The only HEADERS framing path: QPACK-encodes after the reserved frame
    // header room, enforces the encoded-byte limit on the section itself and
    // then writes the frame header immediately before the section.
    [[nodiscard]] static std::variant<http3_headers_frame, http3_field_section_error> encode(
        std::span<const http3_field_section_field_view> fields_value, std::pmr::memory_resource* resource,
        http3_field_section_limits limits, http3_qpack_encoder* encoder, std::uint64_t stream_id,
        std::size_t decoded_field_section_size) {
        constexpr auto reserved = http3_headers_frame::reserved_prefix_bytes;
        std::variant<std::pmr::vector<char>, http3_field_section_error> encoded =
            http3_field_section_error::qpack_encoding_failed;
        if (encoder == nullptr) {
            encoded = encode_static_http3_field_section(fields_value, resource, reserved);
        } else {
            auto result_value = encoder->encode_after_prefix(stream_id, fields_value, limits, resource, reserved);
            if ((result_value.index() != 0)) {
                return std::get<1>(result_value) == http3_qpack_connection_error::limit
                           ? http3_field_section_error::field_section_too_large
                           : http3_field_section_error::qpack_encoding_failed;
            }
            encoded = std::move(std::get<0>(result_value));
        }
        if ((encoded.index() != 0)) {
            return std::get<1>(encoded);
        }
        auto& bytes_value = std::get<0>(encoded);
        const auto section_size = bytes_value.size() - reserved;
        if (section_size > limits.max_encoded_bytes_ || section_size > http3_var_int_max) {
            return http3_field_section_error::field_section_too_large;
        }
        const auto header_size = 1 + http3_var_int_encoded_size(section_size);
        const auto offset = reserved - header_size;
        const auto header_value = encode_http3_frame_header(std::span<char>(bytes_value).subspan(offset, header_size),
            static_cast<std::uint64_t>(http3_frame_type::headers), section_size);
        if ((header_value.index() != 0)) {
            return http3_field_section_error::field_section_too_large;
        }
        return http3_headers_frame(std::move(bytes_value), offset, decoded_field_section_size);
    }
};

}  // namespace ruvia::detail
