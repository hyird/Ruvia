#include "http_priority.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>

#include "http2_frame.h"
#include "http_header.h"
#include "http_structured_field.h"

namespace ruvia {

namespace {
using item_type = http_structured_item;

template <typename input_type>
std::variant<std::monostate, http_priority_error> parse_priority_members(http_structured_parser<input_type> parser, http_priority_fields& fields_value) noexcept {
    parser.spaces();
    while (!parser.empty()) {
        const auto key = parser.key();
        if (key.empty()) {
            return http_priority_error::invalid_syntax;
        }
        item_type item{.kind_ = item_type::kind_type::boolean, .boolean_ = true};
        if (parser.consume('=')) {
            item = {};
            if (!parser.member(item, false)) {
                return http_priority_error::invalid_syntax;
            }
        } else if (!parser.parameters()) {
            return http_priority_error::invalid_syntax;
        }
        if (key == "u") {
            fields_value.urgency_ = item.kind_ == item_type::kind_type::integer ? http_priority_urgency::try_from_value(item.integer_) : std::nullopt;
        }
        if (key == "i") {
            fields_value.incremental_ = item.kind_ == item_type::kind_type::boolean ? std::optional(item.boolean_) : std::nullopt;
        }
        parser.ows();
        if (parser.empty()) {
            break;
        }
        if (!parser.consume(',')) {
            return http_priority_error::invalid_syntax;
        }
        parser.ows();
        if (parser.empty()) {
            return http_priority_error::invalid_syntax;
        }
    }
    return {};
}
}  // namespace

std::variant<http_priority_fields, http_priority_error> parse_http_priority(std::string_view value) noexcept {
    http_priority_fields fields;
    if (auto parsed_value = parse_priority_members(http_structured_parser{http_structured_text_input{value}}, fields); parsed_value.index() != 0) {
        return std::get<1>(parsed_value);
    }
    return fields;
}
std::variant<http_priority_fields, http_priority_error> parse_http_priority(const http_header_range_view& headers) noexcept {
    http_priority_fields fields;
    if (auto parsed_value = parse_priority_members(http_structured_parser{http_structured_header_input{headers.begin(), headers.end(), "priority"}}, fields); parsed_value.index() != 0) {
        return std::get<1>(parsed_value);
    }
    return fields;
}
std::variant<std::size_t, http_priority_error> encode_http_priority(std::span<char> output, http_priority_fields fields_value) noexcept {
    std::array<char, 12> bytes_value{};
    std::size_t count = 0;
    if (fields_value.urgency_) {
        bytes_value[count++] = 'u';
        bytes_value[count++] = '=';
        bytes_value[count++] = static_cast<char>('0' + fields_value.urgency_->value());
    }
    if (fields_value.incremental_) {
        if (*fields_value.incremental_) {
            if (count) {
                bytes_value[count++] = ',';
                bytes_value[count++] = ' ';
            }
            bytes_value[count++] = 'i';
        } else {
            if (count) {
                bytes_value[count++] = ',';
            }
            bytes_value[count++] = 'i';
            bytes_value[count++] = '=';
            bytes_value[count++] = '?';
            bytes_value[count++] = '0';
        }
    }
    if (output.size() < count) {
        return http_priority_error::output_too_small;
    }
    std::copy_n(bytes_value.begin(), count, output.begin());
    return count;
}
std::string format_http_priority(http_priority_fields fields_value) {
    std::array<char, 12> buffer{};
    const auto count = std::get<std::size_t>(encode_http_priority(buffer, fields_value));
    return count == 0 ? std::string{} : std::string(buffer.data(), count);
}
std::variant<http_priority_update, http_priority_error> decode_http2_priority_update(byte_view payload_value) noexcept {
    if (payload_value.size() < 4) {
        return http_priority_error::invalid_frame;
    }
    const auto id = ((std::to_integer<std::uint32_t>(payload_value[0]) & 0x7fU) << 24) | (std::to_integer<std::uint32_t>(payload_value[1]) << 16) | (std::to_integer<std::uint32_t>(payload_value[2]) << 8) | std::to_integer<std::uint32_t>(payload_value[3]);
    if (!id) {
        return http_priority_error::invalid_value;
    }
    auto fields_value = parse_http_priority(as_chars(payload_value.subspan(4)));
    if ((fields_value.index() != 0)) {
        return std::get<1>(fields_value);
    }
    return http_priority_update{id, (id & 1U) == 0, std::get<0>(fields_value)};
}
std::variant<std::size_t, http_priority_error> encode_http2_priority_update(mutable_byte_view output, std::uint32_t id, http_priority_fields fields_value) noexcept {
    if (!id || id > 0x7fffffffU) {
        return http_priority_error::invalid_value;
    }
    std::array<char, 12> value{};
    const auto length = encode_http_priority(value, fields_value);
    if ((length.index() != 0)) {
        return std::get<1>(length);
    }
    const auto field_bytes = std::get<0>(length);
    if (output.size() < 13 + field_bytes) {
        return http_priority_error::output_too_small;
    }
    (void)encode_http2_frame_header(output.first(http2_frame_header_bytes), static_cast<std::uint32_t>(4 + field_bytes), http2_frame_type::priority_update, 0, 0);
    for (std::size_t index = 0; index < 4; ++index) {
        output[http2_frame_header_bytes + index] = static_cast<std::byte>((id >> (24 - 8 * index)) & 0xffU);
    }
    for (std::size_t index = 0; index < field_bytes; ++index) {
        output[13 + index] = static_cast<std::byte>(static_cast<unsigned char>(value[index]));
    }
    return 13 + field_bytes;
}
}  // namespace ruvia
