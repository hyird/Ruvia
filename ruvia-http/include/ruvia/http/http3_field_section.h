#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

namespace ruvia {

enum class http3_field_section_error : std::uint8_t {
    need_more_data,
    integer_overflow,
    invalid_prefix,
    nonzero_required_insert_count,
    invalid_base,
    dynamic_reference,
    invalid_index,
    invalid_huffman,
    field_section_too_large,
    field_list_too_large,
    too_many_fields,
    output_too_small,
    callback_stopped,
    qpack_encoding_failed,
};

struct http3_field_section_field_view final {
    std::string_view name_;
    std::string_view value_;
    bool never_indexed_{false};
};

struct http3_field_section_limits final {
    std::size_t max_encoded_bytes_{64 * 1024};
    // RFC 9114 field-list size: name bytes + value bytes + 32 per field.
    std::size_t max_decoded_bytes_{64 * 1024};
    std::size_t max_fields_{256};
};

using http3_field_section_callback_type = bool (*)(void*, http3_field_section_field_view);

// Decodes a QPACK field section with a permanently empty dynamic table. Callback
// views borrow the input and remain valid only for the duration of the callback.
[[nodiscard]] std::variant<std::size_t, http3_field_section_error> decode_http3_field_section(
    std::span<const char> input, http3_field_section_callback_type callback_value, void* context_value,
    http3_field_section_limits limits = {},
    std::pmr::memory_resource* resource = std::pmr::get_default_resource());

// Encodes fields using exact static-table matches, static name references, or
// literals. The returned bytes are owned by the supplied PMR resource.
[[nodiscard]] std::variant<std::pmr::vector<char>, http3_field_section_error> encode_http3_field_section(
    std::span<const http3_field_section_field_view> fields_value, std::pmr::memory_resource* resource);

}  // namespace ruvia
