#include "ruvia/http/http3_request_writer.h"

#include <limits>
#include <string_view>
#include <variant>

#include "ruvia/http/detail/field/http_trailer_fields.h"
#include "ruvia/http/http_header.h"

#include "http3/http3_field_section_encoder.h"

namespace ruvia {
namespace {
using error_type = http3_request_trailer_error;
std::variant<std::pmr::vector<char>, error_type> request_trailers(std::span<const http3_field_section_field_view> fields_value,
    http3_field_section_limits limits, std::pmr::memory_resource* resource, http3_qpack_encoder* encoder, std::uint64_t stream_id) {
    auto* memory = resource ? resource : std::pmr::get_default_resource();
    if (fields_value.size() > limits.max_fields_) {
        return error_type::too_many_fields;
    }
    std::size_t decoded = 0, name_bytes = 0;
    for (const auto& field : fields_value) {
        if (!is_valid_http_header_name(field.name_) || !is_valid_http_header_value(field.value_)) {
            return error_type::invalid_field;
        }
        if (detail::is_forbidden_http_request_trailer_name(field.name_)) {
            return error_type::forbidden_field;
        }
        if (limits.max_decoded_bytes_ < decoded || limits.max_decoded_bytes_ - decoded < 32 ||
            field.name_.size() > limits.max_decoded_bytes_ - decoded - 32 ||
            field.value_.size() > limits.max_decoded_bytes_ - decoded - 32 - field.name_.size()) {
            return error_type::field_list_too_large;
        }
        decoded += 32 + field.name_.size() + field.value_.size();
        name_bytes += field.name_.size();
    }
    std::pmr::vector<char> names(memory);
    names.reserve(name_bytes);
    std::pmr::vector<http3_field_section_field_view> normalized(memory);
    normalized.reserve(fields_value.size());
    for (const auto& field : fields_value) {
        const auto offset = names.size();
        for (const unsigned char ch : field.name_) {
            names.push_back(ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch + ('a' - 'A')) : static_cast<char>(ch));
        }
        normalized.push_back({std::string_view(names.data() + offset, field.name_.size()), field.value_, field.never_indexed_});
    }
    auto encoded = detail::encode_http3_fields(normalized, memory, limits, encoder, stream_id);
    if ((encoded.index() != 0)) {
        return std::get<1>(encoded) == http3_field_section_error::qpack_encoding_failed ? error_type::qpack_encoding_failed : error_type::field_section_too_large;
    }
    if (std::get<0>(encoded).size() > limits.max_encoded_bytes_) {
        return error_type::field_section_too_large;
    }
    return std::move(std::get<0>(encoded));
}
}  // namespace
std::variant<std::pmr::vector<char>, error_type> encode_http3_request_trailers(std::span<const http3_field_section_field_view> fields_value,
    http3_field_section_limits limits, std::pmr::memory_resource* resource) {
    return request_trailers(fields_value, limits, resource, nullptr, 0);
}
std::variant<std::pmr::vector<char>, error_type> encode_http3_request_trailers(http3_qpack_encoder& encoder, std::uint64_t stream_id,
    std::span<const http3_field_section_field_view> fields_value, http3_field_section_limits limits, std::pmr::memory_resource* resource) {
    return request_trailers(fields_value, limits, resource, &encoder, stream_id);
}
}  // namespace ruvia
