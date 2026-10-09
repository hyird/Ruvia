#include "ruvia/http/http_priority.h"

#include <algorithm>
#include <array>

#include "ruvia/http/http3_frames.h"
#include "ruvia/http/http3_peer_streams.h"
#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_header.h"

#include "field/http_structured_fields.h"
#include "http2/http2_frame_codec.h"

namespace ruvia {
namespace {
using item_type = detail::http_structured_item;

class priority_header_input final {
public:
    explicit priority_header_input(std::span<const http_header_view> headers) noexcept
        : headers_(headers) {
        (void)select_next(remaining_);
        normalize();
    }
    [[nodiscard]] bool empty() const noexcept {
        return remaining_.empty();
    }
    [[nodiscard]] char peek() const noexcept {
        return remaining_.front();
    }
    [[nodiscard]] const char* data() const noexcept {
        return remaining_.data();
    }
    void advance() noexcept {
        remaining_.remove_prefix(1);
        normalize();
    }

private:
    [[nodiscard]] bool select_next(std::string_view& value) noexcept {
        while (next_header_ != headers_.size()) {
            const auto& header_value = headers_[next_header_++];
            if (http_ascii_equals_ignore_case(header_value.name(), "priority")) {
                value = header_value.value();
                return true;
            }
        }
        return false;
    }
    void normalize() noexcept {
        while (remaining_.empty()) {
            if (separator_) {
                separator_ = false;
                remaining_ = pending_;
            } else if (select_next(pending_)) {
                // HTTP combines repeated field lines before Structured Field parsing.
                remaining_ = ", ";
                separator_ = true;
            } else {
                return;
            }
        }
    }

    std::span<const http_header_view> headers_;
    std::string_view remaining_;
    std::string_view pending_;
    std::size_t next_header_{0};
    bool separator_{false};
};

template <typename input_type>
std::variant<std::monostate, http_priority_error> parse_priority_members(detail::http_structured_parser<input_type> parser, http_priority_fields& fields_value) noexcept {
    parser.spaces();
    while (!parser.empty()) {
        const auto key = parser.key();
        if (key.empty()) {
            return http_priority_error::invalid_syntax;
        }
        item_type item{.kind_ = item_type::kind_type::boolean, .boolean_ = true};
        if (parser.take('=')) {
            item = {};
            if (!parser.member(item)) {
                return http_priority_error::invalid_syntax;
            }
        } else if (!parser.parameters()) {
            return http_priority_error::invalid_syntax;
        }
        if (key == "u") {
            fields_value.urgency_ = item.kind_ == item_type::kind_type::integer && item.integer_ >= 0 && item.integer_ <= 7
                                        ? std::optional<std::uint8_t>(static_cast<std::uint8_t>(item.integer_))
                                        : std::nullopt;
        }
        if (key == "i") {
            fields_value.incremental_ = item.kind_ == item_type::kind_type::boolean ? std::optional(item.boolean_) : std::nullopt;
        }
        parser.ows();
        if (parser.empty()) {
            break;
        }
        if (!parser.take(',')) {
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
    if (auto parsed_value = parse_priority_members(detail::http_structured_parser{detail::http_structured_text_input{value}}, fields); parsed_value.index() != 0) {
        return std::get<1>(parsed_value);
    }
    return fields;
}
std::variant<http_priority_fields, http_priority_error> parse_http_priority(std::span<const http_header_view> headers) noexcept {
    http_priority_fields fields;
    if (auto parsed_value = parse_priority_members(detail::http_structured_parser{priority_header_input{headers}}, fields); parsed_value.index() != 0) {
        return std::get<1>(parsed_value);
    }
    return fields;
}
std::variant<std::size_t, http_priority_error> encode_http_priority(std::span<char> output, http_priority_fields fields_value) noexcept {
    if (fields_value.urgency_ && *fields_value.urgency_ > 7) {
        return http_priority_error::invalid_value;
    }
    std::array<char, 12> bytes_value{};
    std::size_t count = 0;
    if (fields_value.urgency_) {
        bytes_value[count++] = 'u';
        bytes_value[count++] = '=';
        bytes_value[count++] = static_cast<char>('0' + *fields_value.urgency_);
    }
    if (fields_value.incremental_) {
        if (count) {
            bytes_value[count++] = ',';
            bytes_value[count++] = ' ';
        }
        bytes_value[count++] = 'i';
        bytes_value[count++] = '=';
        bytes_value[count++] = '?';
        bytes_value[count++] = *fields_value.incremental_ ? '1' : '0';
    }
    if (output.size() < count) {
        return http_priority_error::output_too_small;
    }
    std::copy_n(bytes_value.begin(), count, output.begin());
    return count;
}
std::variant<http_priority_update, http_priority_error> decode_http2_priority_update(std::span<const char> payload_value) noexcept {
    if (payload_value.size() < 4) {
        return http_priority_error::invalid_frame;
    }
    const auto id = detail::http2_read31(reinterpret_cast<const unsigned char*>(payload_value.data()));
    if (!id) {
        return http_priority_error::invalid_value;
    }
    auto fields_value = parse_http_priority({payload_value.data() + 4, payload_value.size() - 4});
    if ((fields_value.index() != 0)) {
        return std::get<1>(fields_value);
    }
    return http_priority_update{id, (id & 1U) == 0, std::get<0>(fields_value)};
}
std::variant<std::size_t, http_priority_error> encode_http2_priority_update(std::span<char> output, std::uint32_t id, http_priority_fields fields_value) noexcept {
    if (!id || id > 0x7fffffffU) {
        return http_priority_error::invalid_value;
    }
    std::array<char, 12> value{};
    const auto length = encode_http_priority(value, fields_value);
    if ((length.index() != 0)) {
        return std::get<1>(length);
    }
    if (output.size() < 13 + std::get<0>(length)) {
        return http_priority_error::output_too_small;
    }
    detail::http2_encode_frame_header(output.data(), static_cast<std::uint32_t>(4 + std::get<0>(length)), static_cast<detail::http2_frame_type>(0x10), 0, 0);
    detail::http2_write32(output.data() + 9, id);
    std::copy_n(value.begin(), std::get<0>(length), output.begin() + 13);
    return 13 + std::get<0>(length);
}
std::variant<http_priority_update, http_priority_error> decode_http3_priority_update(std::uint64_t type, std::span<const char> payload_value) noexcept {
    if (type != 0xf0700 && type != 0xf0701) {
        return http_priority_error::invalid_frame;
    }
    const auto id = decode_http3_var_int(payload_value);
    if ((id.index() != 0) || (type == 0xf0700 && !is_http3_request_stream_id(std::get<0>(id).value_))) {
        return http_priority_error::invalid_value;
    }
    const auto value = payload_value.subspan(std::get<0>(id).encoded_bytes_);
    auto fields_value = parse_http_priority({value.data(), value.size()});
    if ((fields_value.index() != 0)) {
        return std::get<1>(fields_value);
    }
    return http_priority_update{std::get<0>(id).value_, type == 0xf0701, std::get<0>(fields_value)};
}
std::variant<std::size_t, http_priority_error> encode_http3_priority_update(std::span<char> output, http_priority_update update) noexcept {
    if (update.element_id_ > http3_var_int_max || (!update.push_ && !is_http3_request_stream_id(update.element_id_))) {
        return http_priority_error::invalid_value;
    }
    std::array<char, 20> payload_value{};
    const auto id = encode_http3_var_int(payload_value, update.element_id_);
    const auto value = encode_http_priority(std::span(payload_value).subspan(std::get<0>(id)), update.fields_);
    if ((value.index() != 0)) {
        return std::get<1>(value);
    }
    const auto size = encode_http3_frame(output, update.push_ ? 0xf0701 : 0xf0700, std::span(payload_value).first(std::get<0>(id) + std::get<0>(value)));
    if ((size.index() != 0)) {
        return http_priority_error::output_too_small;
    }
    return std::get<0>(size);
}
}  // namespace ruvia
