#include "parser/http_header_block_parser.h"

#include <algorithm>

#include "ruvia/http/http_limits.h"

#include "field/field_value_scan.h"
#include "field/http_request_field_validation.h"

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
// section 3.1). On success `cursor` sits on the first header field and the
// block carries the accepted protocol version.
[[nodiscard]] std::optional<http_parse_error> parse_request_line(std::string_view buffer,
    std::size_t headers_end, std::size_t& cursor_value, parsed_request_header_block& block) noexcept {
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
    const auto version = buffer.substr(version_start, cursor_value - version_start);
    cursor_value += 2;

    // HTTP-version = "HTTP/" DIGIT "." DIGIT (RFC 9112 section 2.3); only
    // HTTP/1.x is served. A higher minor version is processed as HTTP/1.1,
    // the highest minor version implemented (RFC 9110 section 6.2).
    if (version.size() != 8 || !version.starts_with("HTTP/") || version[5] < '0' || version[5] > '9' ||
        version[6] != '.' || version[7] < '0' || version[7] > '9') {
        return http_parse_error::invalid_request_line;
    }
    if (version[5] != '1') {
        return http_parse_error::unsupported_http_version;
    }
    block.protocol_version_ = version[7] == '0' ? http_protocol_version::http10 : http_protocol_version::http11;
    return std::nullopt;
}

// What one request header means on HTTP/1: the shared semantic validator
// decides whether the value is acceptable, and the HTTP/1-specific fields are
// parsed into typed state on the block. Finding the bytes is the loop's job
// below; every rule about what they mean is here.
[[nodiscard]] std::optional<http_parse_error> apply_request_header(request_header_kind kind,
    std::string_view value, bool ignore_upgrade, parsed_request_header_block& block) {
    if (validate_request_field(kind, value, http_field_list_role::recipient, block.fields_) !=
        request_field_status::ok) {
        return kind == request_header_kind::host ? http_parse_error::invalid_host
                                                 : http_parse_error::invalid_header;
    }
    switch (kind) {
        case request_header_kind::content_length:
            switch (block.content_length_.parse_field(value)) {
                case http_content_length_parse_status::ok:
                    break;
                case http_content_length_parse_status::invalid:
                    return http_parse_error::invalid_content_length;
                case http_content_length_parse_status::conflicting:
                    return http_parse_error::conflicting_content_length;
            }
            break;
        case request_header_kind::transfer_encoding:
            switch (block.transfer_encoding_.parse_field(value)) {
                case http_transfer_encoding_parse_status::ok:
                    break;
                case http_transfer_encoding_parse_status::malformed:
                    return http_parse_error::invalid_transfer_encoding;
                case http_transfer_encoding_parse_status::unsupported:
                    break;
            }
            break;
        case request_header_kind::connection:
            if (block.connection_options_.parse_field(value, http_field_list_role::recipient) !=
                http_field_list_parse_status::ok) {
                return http_parse_error::invalid_connection;
            }
            break;
        case request_header_kind::expect:
            block.expectations_.parse_field(value);
            break;
        case request_header_kind::upgrade:
            // RFC 9110 section 7.8 requires a server to ignore Upgrade in
            // an HTTP/1.0 request. The bytes still have to be a valid
            // generic field value, but Upgrade-specific grammar must not
            // turn an otherwise valid HTTP/1.0 request into a 400.
            if (!ignore_upgrade &&
                http_upgrade_protocols{}.parse_field(value, http_field_list_role::recipient) !=
                    http_field_list_parse_status::ok) {
                return http_parse_error::invalid_upgrade;
            }
            break;
        case request_header_kind::accept_encoding:
            block.response_coding_qualities_.update(value);
            break;
        default:
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
        const auto name_start = cursor_value;
        cursor_value += http_token_prefix_size({buffer.data() + cursor_value, headers_end - cursor_value});
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

        const auto kind = classify_request_header(name);
        if (const auto error = apply_request_header(kind, value, ignore_upgrade, block)) {
            return error;
        }

        block.headers_.append(
            parsed_request_header_slot{.name_ = make_slice(name_start, name_end - name_start),
                .value_ = make_slice(value_start, value_end - value_start),
                .kind_ = kind});
        if (kind == request_header_kind::host) {
            block.host_header_index_ = static_cast<known_request_header_index_type>(block.headers_.size() - 1);
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

std::size_t http_request_leading_empty_line_bytes(std::string_view buffer, std::size_t skipped) noexcept {
    buffer = buffer.substr(0, max_http_header_bytes);
    auto cursor_value = std::min(skipped, buffer.size());
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

std::size_t find_http_request_head_end(
    std::string_view buffer, std::size_t& leading_bytes, std::size_t search_offset) noexcept {
    leading_bytes = http_request_leading_empty_line_bytes(buffer, leading_bytes);
    if (leading_bytes >= max_http_header_bytes) {
        return std::string_view::npos;
    }
    const auto head_end = find_http_header_end(
        buffer.substr(leading_bytes, max_http_header_bytes - leading_bytes),
        search_offset > leading_bytes ? search_offset - leading_bytes : 0);
    return head_end == std::string_view::npos ? head_end : leading_bytes + head_end;
}

std::optional<http_parse_error> classify_http_request_line_prefix(std::string_view buffer,
    std::size_t leading_bytes, std::size_t search_offset, std::size_t& request_line_end) noexcept {
    buffer = buffer.substr(0, max_http_header_bytes);
    if (leading_bytes >= buffer.size()) {
        return std::nullopt;
    }
    // The leading scan stops at a lone CR that may still become an empty line.
    if (buffer[leading_bytes] == '\r' && leading_bytes + 1 == buffer.size()) {
        return std::nullopt;
    }
    if (!is_http_token_char(static_cast<unsigned char>(buffer[leading_bytes]))) {
        return http_parse_error::invalid_request_line;
    }
    const auto line_feed = buffer.find('\n', std::max(leading_bytes, search_offset));
    if (line_feed == std::string_view::npos) {
        return std::nullopt;
    }
    parsed_request_header_block block;
    auto cursor_value = leading_bytes;
    if (const auto error = parse_request_line(buffer, line_feed + 1, cursor_value, block)) {
        return error;
    }
    request_line_end = cursor_value;
    return std::nullopt;
}

std::optional<http_parse_error> parse_http_header_block(std::string_view buffer,
    std::size_t leading_bytes, std::size_t header_bytes, parsed_request_header_block& block) {
    const auto headers_end = header_bytes - 2;
    auto cursor_value = leading_bytes;
    if (const auto error = parse_request_line(buffer, headers_end, cursor_value, block)) {
        return error;
    }
    return parse_header_fields(
        buffer, headers_end, cursor_value, block.protocol_version_ == http_protocol_version::http10, block);
}

}  // namespace ruvia::detail
