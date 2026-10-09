#pragma once

#include <cstdint>
#include <memory_resource>
#include <span>
#include <variant>
#include <vector>

#include "ruvia/http/http3_field_section.h"

namespace ruvia {
class http3_qpack_encoder;
enum class http3_request_trailer_error : std::uint8_t {
    invalid_field,
    forbidden_field,
    too_many_fields,
    field_list_too_large,
    field_section_too_large,
    qpack_encoding_failed,
};
// QPACK payload for a request's final trailing HEADERS. Fields borrow this call;
// output uses resource, which must outlive the returned bytes. Send a HEADERS
// frame with FIN after the request DATA plan permits completion. CONNECT tunnels
// do not carry trailers. The transport owns the head/DATA/trailer write ordering.
[[nodiscard]] std::variant<std::pmr::vector<char>, http3_request_trailer_error> encode_http3_request_trailers(
    std::span<const http3_field_section_field_view> fields_value, http3_field_section_limits limits = {},
    std::pmr::memory_resource* resource = std::pmr::get_default_resource());
[[nodiscard]] std::variant<std::pmr::vector<char>, http3_request_trailer_error> encode_http3_request_trailers(
    http3_qpack_encoder& encoder, std::uint64_t stream_id, std::span<const http3_field_section_field_view> fields_value,
    http3_field_section_limits limits = {}, std::pmr::memory_resource* resource = std::pmr::get_default_resource());
}  // namespace ruvia
