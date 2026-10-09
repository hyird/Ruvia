#pragma once

#include <variant>

#include "ruvia/http/http3_field_section.h"
#include "ruvia/http/http3_qpack_connection.h"

namespace ruvia::detail {

inline std::variant<std::pmr::vector<char>, http3_field_section_error> encode_http3_fields(
    std::span<const http3_field_section_field_view> fields_value, std::pmr::memory_resource* resource,
    http3_field_section_limits limits, http3_qpack_encoder* encoder, std::uint64_t stream_id) {
    if (!encoder) {
        return encode_http3_field_section(fields_value, resource);
    }
    auto result_value = encoder->encode(stream_id, fields_value, limits, resource);
    if ((result_value.index() != 0)) {
        return std::get<1>(result_value) == http3_qpack_connection_error::limit
                   ? http3_field_section_error::field_section_too_large
                   : http3_field_section_error::qpack_encoding_failed;
    }
    return std::move(std::get<0>(result_value));
}

}  // namespace ruvia::detail
