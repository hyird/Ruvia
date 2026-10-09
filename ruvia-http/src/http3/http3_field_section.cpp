#include "ruvia/http/http3_field_section.h"

#include <array>
#include <memory_resource>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/http/http3_qpack.h"

#include "http3/qpack_static_table.h"

namespace ruvia {
namespace {

http3_field_section_error map_error(http3_qpack_error error) noexcept {
    switch (error) {
        case http3_qpack_error::need_more_data:
            return http3_field_section_error::need_more_data;
        case http3_qpack_error::integer_overflow:
            return http3_field_section_error::integer_overflow;
        case http3_qpack_error::invalid_index:
            return http3_field_section_error::invalid_index;
        case http3_qpack_error::invalid_huffman:
            return http3_field_section_error::invalid_huffman;
        case http3_qpack_error::output_too_small:
            return http3_field_section_error::output_too_small;
    }
    return http3_field_section_error::invalid_prefix;
}

std::variant<std::size_t, http3_field_section_error> append_integer(std::pmr::vector<char>& output,
    std::uint8_t prefix_bits, std::uint8_t prefix, std::uint64_t value) {
    std::array<char, 11> bytes_value{};
    const auto encoded = encode_http3_qpack_integer(bytes_value, prefix_bits, prefix, value);
    if ((encoded.index() != 0)) {
        return map_error(std::get<1>(encoded));
    }
    output.insert(output.end(), bytes_value.begin(), bytes_value.begin() + static_cast<std::ptrdiff_t>(std::get<0>(encoded)));
    return std::get<0>(encoded);
}

std::variant<std::monostate, http3_field_section_error> append_literal(std::pmr::vector<char>& output,
    std::string_view value, std::uint8_t prefix_bits, std::uint8_t first_byte_flags) {
    const auto length = append_integer(output, prefix_bits, first_byte_flags, value.size());
    if ((length.index() != 0)) {
        return std::get<1>(length);
    }
    output.insert(output.end(), value.begin(), value.end());
    return {};
}

}  // namespace

std::variant<std::size_t, http3_field_section_error> decode_http3_field_section(
    std::span<const char> input, http3_field_section_callback_type callback_value, void* context_value,
    http3_field_section_limits limits, std::pmr::memory_resource* resource) {
    if (input.size() > limits.max_encoded_bytes_) {
        return http3_field_section_error::field_section_too_large;
    }
    if (input.size() < 2) {
        return http3_field_section_error::need_more_data;
    }
    if (static_cast<std::uint8_t>(input[0]) != 0) {
        return http3_field_section_error::nonzero_required_insert_count;
    }
    if (static_cast<std::uint8_t>(input[1]) != 0) {
        return http3_field_section_error::invalid_base;
    }

    std::size_t offset = 2;
    std::size_t decoded_bytes = 0;
    std::size_t field_count = 0;
    std::pmr::string name(resource != nullptr ? resource : std::pmr::get_default_resource());
    std::pmr::string value(resource != nullptr ? resource : std::pmr::get_default_resource());
    const auto remaining = [&] { return input.subspan(offset); };
    const auto add_field = [&](http3_field_section_field_view field) -> std::variant<std::monostate, http3_field_section_error> {
        if (field_count >= limits.max_fields_) {
            return http3_field_section_error::too_many_fields;
        }
        constexpr std::size_t field_overhead = 32;
        const auto remaining_budget = limits.max_decoded_bytes_ - decoded_bytes;
        if (remaining_budget < field_overhead ||
            field.name_.size() > remaining_budget - field_overhead ||
            field.value_.size() > remaining_budget - field_overhead - field.name_.size()) {
            return http3_field_section_error::field_list_too_large;
        }
        decoded_bytes += field_overhead + field.name_.size() + field.value_.size();
        ++field_count;
        if (callback_value != nullptr && !callback_value(context_value, field)) {
            return http3_field_section_error::callback_stopped;
        }
        return {};
    };

    while (offset < input.size()) {
        const auto first = static_cast<std::uint8_t>(input[offset]);
        http3_field_section_field_view field{};
        if ((first & 0x80U) != 0) {
            if ((first & 0x40U) == 0) {
                return http3_field_section_error::dynamic_reference;
            }
            const auto index = decode_http3_qpack_integer(remaining(), 6);
            if ((index.index() != 0)) {
                return map_error(std::get<1>(index));
            }
            const auto entry_value = get_http3_qpack_static_entry(std::get<0>(index).value_);
            if ((entry_value.index() != 0)) {
                return http3_field_section_error::invalid_index;
            }
            offset += std::get<0>(index).encoded_bytes_;
            field = {std::get<0>(entry_value).name_, std::get<0>(entry_value).value_, false};
        } else if ((first & 0xc0U) == 0x40U) {
            const bool never_indexed = (first & 0x20U) != 0;
            if ((first & 0x10U) == 0) {
                return http3_field_section_error::dynamic_reference;
            }
            const auto index = decode_http3_qpack_integer(remaining(), 4);
            if ((index.index() != 0)) {
                return map_error(std::get<1>(index));
            }
            const auto entry_value = get_http3_qpack_static_entry(std::get<0>(index).value_);
            if ((entry_value.index() != 0)) {
                return http3_field_section_error::invalid_index;
            }
            offset += std::get<0>(index).encoded_bytes_;
            const auto consumed = decode_http3_qpack_string(remaining(), value);
            if ((consumed.index() != 0)) {
                return map_error(std::get<1>(consumed));
            }
            offset += std::get<0>(consumed);
            field = {std::get<0>(entry_value).name_, value, never_indexed};
        } else if ((first & 0xe0U) == 0x20U) {
            const bool never_indexed = (first & 0x10U) != 0;
            const auto name_size = decode_http3_qpack_string(remaining(), 3, name);
            if ((name_size.index() != 0)) {
                return map_error(std::get<1>(name_size));
            }
            offset += std::get<0>(name_size);
            const auto value_size = decode_http3_qpack_string(remaining(), value);
            if ((value_size.index() != 0)) {
                return map_error(std::get<1>(value_size));
            }
            offset += std::get<0>(value_size);
            field = {name, value, never_indexed};
        } else {
            return http3_field_section_error::invalid_prefix;
        }
        const auto accepted = add_field(field);
        if ((accepted.index() != 0)) {
            return std::get<1>(accepted);
        }
    }
    return field_count;
}

std::variant<std::pmr::vector<char>, http3_field_section_error> encode_http3_field_section(
    std::span<const http3_field_section_field_view> fields_value, std::pmr::memory_resource* resource) {
    std::pmr::vector<char> output(2, '\0', resource != nullptr ? resource : std::pmr::get_default_resource());
    for (const auto& field : fields_value) {
        const auto match = detail::qpack_static_fields.find(field.name_,
            field.never_indexed_ ? std::nullopt : std::optional(field.value_));
        if (match && match->exact_index_) {
            const auto encoded = append_integer(output, 6, 0xc0, *match->exact_index_);
            if ((encoded.index() != 0)) {
                return std::get<1>(encoded);
            }
        } else if (match) {
            const auto encoded = append_integer(output, 4,
                static_cast<std::uint8_t>(0x50U | (field.never_indexed_ ? 0x20U : 0U)), match->name_index_);
            if ((encoded.index() != 0)) {
                return std::get<1>(encoded);
            }
            const auto value = append_literal(output, field.value_, 7, 0);
            if ((value.index() != 0)) {
                return std::get<1>(value);
            }
        } else {
            const auto name = append_literal(output, field.name_, 3,
                static_cast<std::uint8_t>(0x20U | (field.never_indexed_ ? 0x10U : 0U)));
            if ((name.index() != 0)) {
                return std::get<1>(name);
            }
            const auto value = append_literal(output, field.value_, 7, 0);
            if ((value.index() != 0)) {
                return std::get<1>(value);
            }
        }
    }
    return output;
}

}  // namespace ruvia
