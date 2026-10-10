#include "parser/http_header_block_parser.h"

#include <algorithm>

#include "ruvia/http/detail/field/http_trailer_fields.h"
#include "ruvia/http/http_limits.h"

#include "coding/http_content_coding.h"
#include "field/field_value_scan.h"
#include "field/http_cors_fields.h"
#include "field/http_media_type.h"
#include "field/http_origin_fields.h"
#include "field/http_te_fields.h"
#include "parser/http_request_target.h"

namespace ruvia::detail {
namespace {

[[nodiscard]] http_header_slice make_slice(std::size_t offset, std::size_t length) noexcept {
    return http_header_slice{
        .offset_ = static_cast<std::uint32_t>(offset), .length_ = static_cast<std::uint32_t>(length)};
}

[[nodiscard]] std::size_t trim_right_ows(
    std::string_view buffer, std::size_t begin, std::size_t end) noexcept {
    while (end > begin && (buffer[end - 1] == ' ' || buffer[end - 1] == '\t')) {
        --end;
    }
    return end;
}

// The request line: method SP request-target SP HTTP-version CRLF (RFC 9112
// section 3.1). On success `cursor` sits on the first header field, and
// `ignore_upgrade` reports an HTTP/1.0 version, which changes how the field
// section treats Upgrade.
[[nodiscard]] std::optional<http_parse_error> parse_request_line(std::string_view buffer,
    std::size_t headers_end, std::size_t& cursor_value, parsed_request_header_block& block,
    bool& ignore_upgrade) noexcept {
    const auto method_start = cursor_value;
    while (cursor_value < headers_end && is_http_token_char(static_cast<unsigned char>(buffer[cursor_value]))) {
        ++cursor_value;
    }
    if (cursor_value == method_start || cursor_value >= headers_end || buffer[cursor_value] != ' ') {
        return http_parse_error::invalid_request_line;
    }
    block.method_ = make_slice(method_start, cursor_value - method_start);
    ++cursor_value;

    const auto target_start = cursor_value;
    while (cursor_value < headers_end && buffer[cursor_value] != ' ') {
        if (buffer[cursor_value] == '\r' || buffer[cursor_value] == '\n') {
            return http_parse_error::invalid_request_line;
        }
        ++cursor_value;
    }
    if (cursor_value == target_start || cursor_value >= headers_end) {
        return http_parse_error::invalid_request_line;
    }
    block.target_ = make_slice(target_start, cursor_value - target_start);
    ++cursor_value;

    const auto version_start = cursor_value;
    while (cursor_value < headers_end && buffer[cursor_value] != '\r') {
        if (buffer[cursor_value] == '\n' || buffer[cursor_value] == ' ') {
            return http_parse_error::invalid_request_line;
        }
        ++cursor_value;
    }
    if (cursor_value == version_start || cursor_value + 1 >= headers_end || buffer[cursor_value + 1] != '\n') {
        return http_parse_error::invalid_request_line;
    }
    block.version_ = make_slice(version_start, cursor_value - version_start);
    ignore_upgrade = buffer.substr(version_start, cursor_value - version_start) == "HTTP/1.0";
    cursor_value += 2;

    return std::nullopt;
}

// What one known request header means: which values are validated at the
// protocol boundary, which are parsed into a typed field on the block, and
// which may appear only once. Finding the bytes is the loop's job below; every
// rule about what they mean is here.
[[nodiscard]] std::optional<http_parse_error> apply_request_header(request_header_kind kind,
    std::string_view value, bool ignore_upgrade, parsed_request_header_block& block) {
    switch (kind) {
        case request_header_kind::host:
            if (block.host_header_index_ >= 0 || !is_valid_host_header(value)) {
                return http_parse_error::invalid_host;
            }
            break;
        case request_header_kind::content_length: {
            switch (block.content_length_.parse_field(value)) {
                case http_content_length_parse_status::ok:
                    break;
                case http_content_length_parse_status::invalid:
                    return http_parse_error::invalid_content_length;
                case http_content_length_parse_status::conflicting:
                    return http_parse_error::conflicting_content_length;
            }
            break;
        }
        case request_header_kind::transfer_encoding: {
            switch (block.transfer_encoding_.parse_field(value)) {
                case http_transfer_encoding_parse_status::ok:
                    break;
                case http_transfer_encoding_parse_status::malformed:
                    return http_parse_error::invalid_transfer_encoding;
                case http_transfer_encoding_parse_status::unsupported:
                    break;
            }
            break;
        }
        case request_header_kind::connection: {
            if (block.connection_options_.parse_field(
                    value, http_field_list_role::recipient, [](std::string_view option) noexcept {
                        return !http_connection_option_conflicts_with_managed_field(option);
                    }) != http_field_list_parse_status::ok) {
                return http_parse_error::invalid_connection;
            }
            break;
        }
        case request_header_kind::expect: {
            if (!is_valid_received_http_expect_field_value(value)) {
                return http_parse_error::invalid_header;
            }
            block.expectations_.parse_field(value);
            break;
        }
        case request_header_kind::upgrade:
            // RFC 9110 section 7.8 requires a server to ignore Upgrade in
            // an HTTP/1.0 request. The bytes still have to be a valid
            // generic field value, but Upgrade-specific grammar must not
            // turn an otherwise valid HTTP/1.0 request into a 400.
            if (!ignore_upgrade &&
                block.upgrade_protocols_.parse_field(value, http_field_list_role::recipient,
                    [](const http_upgrade_protocol&) noexcept { return true; }) !=
                    http_field_list_parse_status::ok) {
                return http_parse_error::invalid_upgrade;
            }
            break;
        case request_header_kind::accept_encoding:
            block.response_coding_qualities_.update(value);
            break;
        case request_header_kind::access_control_request_method:
            if (!is_valid_http_cors_request_method(value)) {
                return http_parse_error::invalid_header;
            }
            if (const auto bit = singleton_request_header_bit(kind);
                (block.seen_header_bits_ & bit) != 0) {
                return http_parse_error::invalid_header;
            } else {
                block.seen_header_bits_ |= bit;
            }
            break;
        case request_header_kind::origin:
            if (!is_valid_http_origin_field_value(value)) {
                return http_parse_error::invalid_header;
            }
            if (const auto bit = singleton_request_header_bit(kind);
                (block.seen_header_bits_ & bit) != 0) {
                return http_parse_error::invalid_header;
            } else {
                block.seen_header_bits_ |= bit;
            }
            break;
        case request_header_kind::content_type: {
            // Content-Type is a typed field, not an arbitrary singleton.
            // Validate its media-type grammar at the protocol boundary so
            // recipients and both request writers accept the same values.
            if (!is_valid_http_content_type_field_value(value)) {
                return http_parse_error::invalid_header;
            }
            const auto bit = singleton_request_header_bit(kind);
            if ((block.seen_header_bits_ & bit) != 0) {
                return http_parse_error::invalid_header;
            }
            block.seen_header_bits_ |= bit;
            break;
        }
        case request_header_kind::content_encoding:
            if (!is_valid_http_content_encoding_field_value(value, http_field_list_role::recipient)) {
                return http_parse_error::invalid_header;
            }
            break;
        case request_header_kind::authorization:
        case request_header_kind::if_match:
        case request_header_kind::if_modified_since:
        case request_header_kind::if_none_match:
        case request_header_kind::if_range:
        case request_header_kind::if_unmodified_since:
        case request_header_kind::range:
        case request_header_kind::sec_websocket_key:
        case request_header_kind::sec_websocket_version:
        case request_header_kind::user_agent:
            if (const auto bit = singleton_request_header_bit(kind); bit != 0) {
                if ((block.seen_header_bits_ & bit) != 0) {
                    return http_parse_error::invalid_header;
                }
                block.seen_header_bits_ |= bit;
            }
            break;
        case request_header_kind::other:
        case request_header_kind::accept:
        case request_header_kind::cookie:
        case request_header_kind::sec_websocket_protocol:
        case request_header_kind::forwarded:
        case request_header_kind::x_forwarded_for:
        case request_header_kind::x_forwarded_proto:
        case request_header_kind::sec_websocket_extensions:
            break;
        case request_header_kind::access_control_request_headers:
            if (!is_valid_http_cors_request_header_names(value)) {
                return http_parse_error::invalid_header;
            }
            break;
    }

    return std::nullopt;
}

// The header section: one field line at a time, name and value delimited by
// ':' and CRLF with optional whitespace trimmed off the value.
[[nodiscard]] std::optional<http_parse_error> parse_header_fields(std::string_view buffer,
    std::size_t headers_end, std::size_t cursor_value, bool ignore_upgrade,
    parsed_request_header_block& block) {
    while (cursor_value < headers_end) {
        if (block.header_count_ == max_http_header_fields) {
            return http_parse_error::too_many_headers;
        }

        const auto name_start = cursor_value;
        while (cursor_value < headers_end && is_http_token_char(static_cast<unsigned char>(buffer[cursor_value]))) {
            ++cursor_value;
        }
        if (cursor_value == name_start || cursor_value >= headers_end || buffer[cursor_value] != ':') {
            return http_parse_error::invalid_header;
        }
        const auto name_end = cursor_value;
        ++cursor_value;

        while (cursor_value < headers_end && (buffer[cursor_value] == ' ' || buffer[cursor_value] == '\t')) {
            ++cursor_value;
        }
        const auto value_start = cursor_value;
        cursor_value += http_field_value_prefix_size({buffer.data() + cursor_value, headers_end - cursor_value});
        if (cursor_value + 1 >= headers_end || buffer[cursor_value] != '\r' || buffer[cursor_value + 1] != '\n') {
            return http_parse_error::invalid_header;
        }
        const auto value_end = trim_right_ows(buffer, value_start, cursor_value);

        const auto name = buffer.substr(name_start, name_end - name_start);
        const auto value = buffer.substr(value_start, value_end - value_start);

        if (http_ascii_equals_ignore_case(name, "Trailer")) {
            if (!is_valid_http_request_trailer_field_value(value, http_field_list_role::recipient)) {
                return http_parse_error::invalid_header;
            }
            if (!http_find_header_token(value, [](std::string_view) noexcept {
                    return true;
                }).empty()) {
                block.non_empty_trailer_header_present_ = true;
            }
        }
        if (http_ascii_equals_ignore_case(name, "TE")) {
            if (!is_valid_received_http_te_field_value(value)) {
                return http_parse_error::invalid_header;
            }
            block.te_header_present_ = true;
        }

        const auto kind = classify_request_header(name);
        if (const auto error = apply_request_header(kind, value, ignore_upgrade, block)) {
            return error;
        }

        const auto index = block.header_count_++;
        block.headers_[index] =
            parsed_request_header_slot{.name_ = make_slice(name_start, name_end - name_start),
                .value_ = make_slice(value_start, value_end - value_start),
                .kind_ = kind};
        if (kind == request_header_kind::host) {
            block.host_header_index_ = static_cast<known_request_header_index_type>(index);
        }
        cursor_value += 2;
    }

    return std::nullopt;
}

}  // namespace

std::size_t find_http_header_end(std::string_view buffer, std::size_t search_offset) noexcept {
    buffer = buffer.substr(0, max_http_header_bytes);
    if (buffer.size() < 4) {
        return std::string_view::npos;
    }

    auto cursor_value = std::max<std::size_t>(3, search_offset);
    while (cursor_value < buffer.size()) {
        const auto i = buffer[cursor_value] == '\n' ? cursor_value : buffer.find('\n', cursor_value + 1);
        if (i == std::string_view::npos) {
            return std::string_view::npos;
        }
        if (buffer[i - 1] == '\r' && buffer[i - 2] == '\n' && buffer[i - 3] == '\r') {
            return i + 1;
        }
        cursor_value = i + 1;
    }

    return std::string_view::npos;
}

std::size_t http_request_leading_empty_line_bytes(std::string_view buffer) noexcept {
    buffer = buffer.substr(0, max_http_header_bytes);
    std::size_t cursor_value = 0;
    for (;;) {
        if (cursor_value < buffer.size() && buffer[cursor_value] == '\n') {
            ++cursor_value;
        } else if (buffer.substr(cursor_value).starts_with("\r\n")) {
            cursor_value += 2;
        } else {
            return cursor_value;
        }
    }
}

std::size_t find_http_request_head_end(std::string_view buffer, std::size_t search_offset) noexcept {
    const auto leading_bytes = http_request_leading_empty_line_bytes(buffer);
    if (leading_bytes >= max_http_header_bytes) {
        return std::string_view::npos;
    }
    const auto head_end = find_http_header_end(
        buffer.substr(leading_bytes, max_http_header_bytes - leading_bytes),
        search_offset > leading_bytes ? search_offset - leading_bytes : 0);
    return head_end == std::string_view::npos ? head_end : leading_bytes + head_end;
}

std::optional<http_parse_error> parse_http_header_block(
    std::string_view buffer, std::size_t header_bytes, parsed_request_header_block& block) {
    const auto headers_end = header_bytes - 2;
    std::size_t cursor_value = http_request_leading_empty_line_bytes(buffer.substr(0, header_bytes));
    bool ignore_upgrade = false;
    if (const auto error = parse_request_line(buffer, headers_end, cursor_value, block, ignore_upgrade)) {
        return error;
    }
    return parse_header_fields(buffer, headers_end, cursor_value, ignore_upgrade, block);
}

}  // namespace ruvia::detail
