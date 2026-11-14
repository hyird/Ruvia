#include "http_response_server.h"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#include "borrowed_text.h"
#include "http_client_protocol.h"
#include "http_connection_fields.h"
#include "http_header.h"
#include "http_request.h"
#include "http_response.h"
#include "websocket_handshake.h"

namespace ruvia {
namespace {

using ruvia::detail::classify_http_request_line_prefix;
using ruvia::detail::find_http_request_head_end;
using ruvia::detail::has_http_bare_line_feed;
using ruvia::detail::http_request_access;
using ruvia::detail::parse_http_header_block;
using ruvia::detail::parse_request_target;
using ruvia::detail::parsed_request_header_block;
using ruvia::detail::request_target_view;

// The head cap hit before the request-line ended. Bytes past the method's SP
// with no further SP or line end are all request-target, which RFC 9112
// section 3 answers with 414 rather than a header-size rejection.
[[nodiscard]] http_parse_error classify_head_cap(std::string_view buffer, std::size_t leading_bytes, std::size_t request_line_end) noexcept {
    buffer = buffer.substr(0, ruvia::max_http_header_bytes);
    if (request_line_end != 0 || leading_bytes >= buffer.size()) {
        return http_parse_error::header_too_large;
    }
    const auto line = buffer.substr(leading_bytes);
    const auto method_end = line.find(' ');
    if (method_end == std::string_view::npos || line.find_first_of(" \r\n", method_end + 1) != std::string_view::npos) {
        return http_parse_error::header_too_large;
    }
    return http_parse_error::uri_too_long;
}

void apply_parsed_request_target(http_request& request, const request_target_view& target_view) noexcept {
    http_request_access::set_path(request, target_view.path_);
    http_request_access::set_query_string(request, target_view.query_);
    http_request_access::set_scheme(request, target_view.scheme_);
    http_request_access::set_authority(request, target_view.authority_);
    http_request_access::set_target_form(request, target_view.form_);
}

// RFC 9112 sections 3.2.2 and 3.3 make the request-target authoritative for
// absolute-form and authority-form. Host field lines bind to this value in
// both headers() and the known-header cache so application code cannot
// observe a conflicting Host value as a second routing truth.
[[nodiscard]] std::string_view bound_host_field_value(const request_target_view& target_view, std::string_view value) noexcept {
    return target_view.form_ == http_request_target_form::absolute || target_view.form_ == http_request_target_form::authority ? target_view.authority_ : value;
}

// True if a whitespace-separated word of `line` looks like an HTTP-version:
// "HTTP/" in any case, or token "/" 1*DIGIT "." 1*DIGIT.
[[nodiscard]] bool names_http_version(std::string_view line) noexcept {
    while (!line.empty()) {
        const auto word_end = line.find_first_of(" \t\r\n");
        const auto word = line.substr(0, word_end);
        if (word.size() >= 5 && http_ascii_equals_ignore_case(word.substr(0, 5), "HTTP/")) {
            return true;
        }
        const auto name_size = http_token_prefix_size(word);
        const auto version = word.substr(name_size);
        const auto dot = version.find('.');
        const auto digits = [](std::string_view value) noexcept {
            return !value.empty() && std::ranges::all_of(value, [](char ch) noexcept {
                return ch >= '0' && ch <= '9';
            });
        };
        if (name_size != 0 && version.starts_with('/') && dot != std::string_view::npos && digits(version.substr(1, dot - 1)) && digits(version.substr(dot + 1))) {
            return true;
        }
        if (word_end == std::string_view::npos) {
            return false;
        }
        line.remove_prefix(word_end + 1);
    }
    return false;
}
}  // namespace

http1_server_request_parse_state::http1_server_request_parse_state() noexcept :
    request_(http_request_access::make()) {}

http_request http1_server_request_parse_state::take_request() && noexcept {
    return std::exchange(request_, http_request_access::make());
}

http1_request_body_plan http1_server_request_parse_state::take_body_plan() && noexcept {
    return std::exchange(body_plan_, http1_request_body_plan(http_request_expectations{}));
}

http1_request_connection_plan http1_server_request_parse_state::take_connection_plan() && noexcept {
    return std::exchange(connection_plan_, http1_request_connection_plan::http11_close());
}

void http1_server_request_parse_state::parse_head(std::string_view buffer, std::size_t header_search_offset) {
    // Bytes before header_search_offset were classified by an earlier call on
    // this state; an earlier search offset discards that progress.
    auto scan = need_request_head() != nullptr ? *need_request_head() : http1_server_need_request_head{};
    scan.leading_bytes_ = std::min(scan.leading_bytes_, header_search_offset);
    scan.request_line_end_ = scan.request_line_end_ <= header_search_offset ? scan.request_line_end_ : 0;

    // Incomplete input allocates no descriptor storage. The scan state is
    // published first so an allocation failure while binding descriptors
    // below cannot leave an earlier head_ready result beside the new request.
    body_plan_ = http1_request_body_plan(http_request_expectations{});
    connection_plan_ = http1_request_connection_plan::http11_close();
    progress_ = scan;
    request_.reset();

    const auto fail = [this](http_parse_error error) noexcept {
        // A rejected head leaves the message boundary unknown and its content
        // unconsumed, so the connection cannot be reused (RFC 9112 sections 6.1
        // and 6.3). The request version may already have been accepted when a
        // later target/framing semantic check fails; preserve it so an HTTP/1.0
        // error response is not silently upgraded to HTTP/1.1.
        const auto connection_plan = connection_plan_.require_close();
        request_.reset();
        progress_ = http1_server_request_parse_failure(error);
        connection_plan_ = connection_plan;
    };

    const auto header_bytes = find_http_request_head_end(buffer, scan.leading_bytes_, header_search_offset);
    if (header_bytes == std::string_view::npos) {
        if (scan.request_line_end_ == 0) {
            if (const auto error = classify_http_request_line_prefix(buffer, scan.leading_bytes_, header_search_offset, scan.request_line_end_)) {
                return fail(*error);
            }
        }
        // A field section line ending in a bare LF can never complete the
        // CRLF-delimited head (RFC 9112 section 2.2); answer it at once rather
        // than waiting for the header timeout.
        if (scan.request_line_end_ != 0 && has_http_bare_line_feed(buffer, std::max(scan.request_line_end_, header_search_offset))) {
            return fail(http_parse_error::invalid_header);
        }
        if (buffer.size() >= max_http_header_bytes) {
            return fail(classify_head_cap(buffer, scan.leading_bytes_, scan.request_line_end_));
        }
        progress_ = scan;
        return;
    }

    parsed_request_header_block block;
    if (const auto error = parse_http_header_block(buffer, scan.leading_bytes_, header_bytes, block)) {
        return fail(*error);
    }

    // parse_http_header_block scans the method through the token table. Preserve the
    // exact wire token: method registration is extensible, while http_known_method is
    // only the framework's routing/response-semantics classification.
    const auto method = block.method_.bind(buffer);
    http_request_access::set_method(request_, method);
    const auto known_method = request_.known_method();

    const auto target = block.target_.bind(buffer);
    http_request_access::set_target(request_, target);

    const auto protocol_version = block.protocol_version_;
    http_request_access::set_protocol_version(request_, protocol_version);
    // Publish the version-specific request contract before any validation that
    // can fail after the version line itself has been accepted. The final
    // disposition is already derived from the parsed Connection fields and is
    // tightened to close by body/response policy later.
    connection_plan_ = protocol_version == http_protocol_version::http11 ? plan_http11_request_connection(block.connection_options_.close()) : plan_http10_request_connection(block.connection_options_.close(), block.connection_options_.keep_alive());
    if (known_method == http_known_method::connect) {
        connection_plan_ = connection_plan_.with_tunnel_request();
    }
    // RFC 9110 10.1.4 obliges the TE sender to list "TE" in Connection; it does
    // not oblige the recipient to reject. The server never acts on TE (it does
    // not condition trailers on it), so a bare TE is simply ignored.

    request_target_view target_view;
    if (!parse_request_target(known_method, target, target_view)) {
        return fail(http_parse_error::invalid_request_target);
    }
    apply_parsed_request_target(request_, target_view);
    if (protocol_version == http_protocol_version::http11 && block.host_header_index_ < 0) {
        return fail(http_parse_error::missing_host);
    }
    const auto host_header_index = block.host_header_index_;

    const auto content_length = block.content_length_.value();
    const auto& transfer_encoding = block.transfer_encoding_.value();
    if (transfer_encoding.has_value() && content_length.has_value()) {
        return fail(http_parse_error::invalid_transfer_encoding);
    }

    if (known_method == http_known_method::connect) {
        // RFC 9110 section 9.3.6: CONNECT content has no defined meaning and
        // bytes after the head belong to the tunnel, so any content framing
        // leaves the message boundary ambiguous. A zero Content-Length frames
        // no content (RFC 9112 section 6.3). TRACE content (section 9.3.8) is
        // a sender prohibition only; its framing is unambiguous, so it is
        // framed and consumed like any other content.
        if (transfer_encoding.has_value()) {
            return fail(http_parse_error::invalid_transfer_encoding);
        }
        if (content_length.has_value() && *content_length != 0) {
            return fail(http_parse_error::invalid_content_length);
        }
    }

    const auto* final_chunked = transfer_encoding.has_value() ? transfer_encoding->final_chunked() : nullptr;
    if (transfer_encoding.has_value() && final_chunked == nullptr) {
        return fail(http_parse_error::invalid_transfer_encoding);
    }

    // RFC 9112 section 6.1: Transfer-Encoding in an HTTP/1.0 request must be treated
    // as faulty framing; the error path closes the connection after replying.
    if (transfer_encoding.has_value() && protocol_version == http_protocol_version::http10) {
        return fail(http_parse_error::invalid_transfer_encoding);
    }
    if (block.transfer_encoding_.unsupported()) {
        return fail(http_parse_error::unsupported_transfer_encoding);
    }

    const auto headers = block.headers_.view();
    http_request_access::reserve_headers(request_, headers.size());
    for (std::size_t i = 0; i < headers.size(); ++i) {
        const auto& header_value = headers[i];
        auto value = header_value.value_.bind(buffer);
        if (host_header_index >= 0 && i == static_cast<std::size_t>(host_header_index)) {
            value = bound_host_field_value(target_view, value);
        }
        if (!http_request_access::add_header(request_, http_header_view{header_value.name_.bind(buffer), value}, header_value.kind_)) {
            return fail(http_parse_error::header_too_large);
        }
    }

    auto expectations = block.expectations_;
    if (protocol_version == http_protocol_version::http10) {
        expectations.ignore_continue();
    }
    if (final_chunked != nullptr) {
        body_plan_ = http1_request_body_plan(final_chunked->transfer_codings(), expectations);
    } else if (content_length.has_value()) {
        if (*content_length > (std::numeric_limits<std::size_t>::max)()) {
            return fail(http_parse_error::invalid_content_length);
        }
        body_plan_ = http1_request_body_plan(static_cast<std::size_t>(*content_length), expectations);
    } else {
        body_plan_ = http1_request_body_plan(expectations);
    }
    progress_ = http1_server_request_head_ready(header_bytes);
}

http_request make_parsed_http_request(borrowed_text method, borrowed_text target, std::span<const http_header_view> headers, borrowed_span<std::byte> body) {
    const auto method_view = method.view();
    const auto target_text = target.view();
    if (!is_valid_http_method_token(method_view)) {
        throw http_parse_protocol_error(http_parse_error::invalid_request_line);
    }
    auto request = http_request_access::make();
    http_request_access::set_method(request, method_view);
    http_request_access::set_target(request, target_text);

    request_target_view target_view;
    if (!parse_request_target(request.known_method(), target_text, target_view)) {
        throw http_parse_protocol_error(http_parse_error::invalid_request_target);
    }
    // The same recipient field rules as the wire parsers: repeated singleton
    // fields such as Range or If-Range make the request invalid instead of
    // resolving to one of their lines.
    http_field_section_budget budget;
    detail::request_field_state fields(detail::request_field_framing::http1);
    static constexpr std::size_t inline_kind_capacity = 32;
    std::array<detail::http_field_kind, inline_kind_capacity> inline_kinds{};
    for (std::size_t i = 0; i < headers.size(); ++i) {
        const auto& header = headers[i];
        if (!is_valid_http_header_name(header.name()) || !is_valid_http_header_value(header.value())) {
            throw http_parse_protocol_error(http_parse_error::invalid_header);
        }
        if (!budget.add_field(header.name(), header.value())) {
            throw http_parse_protocol_error(http_parse_error::header_too_large);
        }
        const auto kind = detail::classify_http_field(header.name());
        if (i < inline_kind_capacity) {
            inline_kinds[i] = kind;
        }
        if (detail::validate_request_field(kind, header.value(), detail::http_field_list_role::recipient, fields) != detail::request_field_status::ok) {
            throw http_parse_protocol_error(kind == detail::http_field_kind::host ? http_parse_error::invalid_host : http_parse_error::invalid_header);
        }
    }

    apply_parsed_request_target(request, target_view);
    http_request_access::reserve_headers(request, headers.size());
    for (std::size_t i = 0; i < headers.size(); ++i) {
        auto field = headers[i];
        const auto kind = i < inline_kind_capacity ? inline_kinds[i] : detail::classify_http_field(field.name());
        if (kind == detail::http_field_kind::host) {
            field = http_header_view{field.name(), bound_host_field_value(target_view, field.value())};
        }
        if (!http_request_access::add_header(request, field, kind)) {
            throw http_parse_protocol_error(http_parse_error::header_too_large);
        }
    }
    http_request_access::set_body(request, body.view());
    return request;
}

bool should_drop_invalid_cleartext_http1_input(std::string_view buffer, http1_server_request_parse_failure_source source_value) noexcept {
    if (source_value != http1_server_request_parse_failure_source::request_line) {
        return false;
    }

    buffer.remove_prefix(detail::http_request_leading_empty_line_bytes(buffer, 0));
    if (buffer.empty()) {
        return false;
    }
    // A first line that names an HTTP version shows the peer speaks HTTP and
    // earns the 400 of RFC 9112 sections 2.2 and 3, even when the line starts
    // with whitespace. Other input (TLS records, SSH banners) is dropped
    // silently; without a complete line, so is input whose first byte cannot
    // start a method.
    const auto line_end = buffer.find("\r\n");
    if (names_http_version(buffer.substr(0, line_end))) {
        return false;
    }
    return line_end != std::string_view::npos || !is_http_token_char(static_cast<unsigned char>(buffer.front()));
}

}  // namespace ruvia

namespace ruvia::detail {
namespace {

[[nodiscard]] http_header_slice make_slice(std::size_t offset, std::size_t length) noexcept {
    return http_header_slice{.offset_ = static_cast<std::uint32_t>(offset), .length_ = static_cast<std::uint32_t>(length)};
}

// The request line: method SP request-target SP HTTP-version CRLF (RFC 9112
// section 3.1). On success `cursor` sits on the first header field and the
// block carries the accepted protocol version.
[[nodiscard]] std::optional<http_parse_error> parse_request_line(std::string_view buffer, std::size_t headers_end, std::size_t& cursor_value, parsed_request_header_block& block) noexcept {
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

    const auto parsed_version = parse_http1_version(version);
    if (parsed_version == http1_version_parse_result::invalid) {
        return http_parse_error::invalid_request_line;
    }
    if (parsed_version == http1_version_parse_result::unsupported) {
        return http_parse_error::unsupported_http_version;
    }
    block.protocol_version_ = parsed_version == http1_version_parse_result::http10 ? http_protocol_version::http10 : http_protocol_version::http11;
    return std::nullopt;
}

// What one request header means on HTTP/1: the shared semantic validator
// decides whether the value is acceptable, and the HTTP/1-specific fields are
// parsed into typed state on the block. Finding the bytes is the loop's job
// below; every rule about what they mean is here.
[[nodiscard]] std::optional<http_parse_error> apply_request_header(http_field_kind kind, std::string_view value, bool ignore_upgrade, parsed_request_header_block& block) {
    if (validate_request_field(kind, value, http_field_list_role::recipient, block.fields_) != request_field_status::ok) {
        return kind == http_field_kind::host ? http_parse_error::invalid_host : http_parse_error::invalid_header;
    }
    switch (kind) {
        case http_field_kind::content_length:
            if (const auto error = parse_http1_content_length_field<http_parse_error>(value, block.content_length_)) {
                return error;
            }
            break;
        case http_field_kind::transfer_encoding:
            if (const auto error = parse_http1_transfer_encoding_field<http_parse_error>(value, block.transfer_encoding_)) {
                return error;
            }
            break;
        case http_field_kind::connection:
            if (block.connection_options_.parse_field(value, http_connection_field_role::request_recipient) != http_field_list_parse_status::ok) {
                return http_parse_error::invalid_connection;
            }
            break;
        case http_field_kind::expect:
            block.expectations_.parse_field(value);
            break;
        case http_field_kind::upgrade:
            // RFC 9110 section 7.8 requires a server to ignore Upgrade in
            // an HTTP/1.0 request. The bytes still have to be a valid
            // generic field value, but Upgrade-specific grammar must not
            // turn an otherwise valid HTTP/1.0 request into a 400.
            if (!ignore_upgrade && http_upgrade_protocols{}.parse_field(value, http_field_list_role::recipient) != http_field_list_parse_status::ok) {
                return http_parse_error::invalid_upgrade;
            }
            break;
        default:
            break;
    }

    return std::nullopt;
}

// The header section: one field line at a time, name and value delimited by
// ':' and CRLF with optional whitespace trimmed off the value.
[[nodiscard]] std::optional<http_parse_error> parse_header_fields(std::string_view buffer, std::size_t headers_end, std::size_t cursor_value, bool ignore_upgrade, parsed_request_header_block& block) {
    buffer = buffer.substr(0, headers_end);
    while (cursor_value < headers_end) {
        // The field-count rule of http_field_section_budget.
        if (block.headers_.size() == max_http_header_fields) {
            return http_parse_error::header_too_large;
        }
        const auto field = scan_http1_field_line(buffer, cursor_value);
        if (!field) {
            return http_parse_error::invalid_header;
        }
        const auto name = field->name();
        const auto value = field->value();

        const auto kind = classify_http_field(name);
        if (const auto error = apply_request_header(kind, value, ignore_upgrade, block)) {
            return error;
        }

        block.headers_.append(parsed_request_header_slot{.name_ = make_slice(static_cast<std::size_t>(name.data() - buffer.data()), name.size()), .value_ = make_slice(static_cast<std::size_t>(value.data() - buffer.data()), value.size()), .kind_ = kind});
        if (kind == http_field_kind::host) {
            block.host_header_index_ = static_cast<known_request_header_index_type>(block.headers_.size() - 1);
        }
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
        if (buffer[i - 1] == '\r') {
            if (buffer[i - 2] == '\n' && buffer[i - 3] == '\r') {
                return i + 1;
            }
            if (i + 2 < buffer.size() && buffer[i + 1] == '\r' && buffer[i + 2] == '\n') {
                return i + 3;
            }
            cursor_value = i + 3;
            continue;
        }
        cursor_value = i + 1;
    }

    return std::string_view::npos;
}

bool has_http_bare_line_feed(std::string_view buffer, std::size_t search_offset) noexcept {
    buffer = buffer.substr(0, max_http_header_bytes);
    for (auto line_feed = buffer.find('\n', std::max<std::size_t>(search_offset, 1)); line_feed != std::string_view::npos; line_feed = buffer.find('\n', line_feed + 1)) {
        if (buffer[line_feed - 1] != '\r') {
            return true;
        }
    }
    return false;
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

std::size_t find_http_request_head_end(std::string_view buffer, std::size_t& leading_bytes, std::size_t search_offset) noexcept {
    leading_bytes = http_request_leading_empty_line_bytes(buffer, leading_bytes);
    if (leading_bytes >= max_http_header_bytes) {
        return std::string_view::npos;
    }
    const auto head_end = find_http_header_end(buffer.substr(leading_bytes, max_http_header_bytes - leading_bytes), search_offset > leading_bytes ? search_offset - leading_bytes : 0);
    return head_end == std::string_view::npos ? head_end : leading_bytes + head_end;
}

std::optional<http_parse_error> classify_http_request_line_prefix(std::string_view buffer, std::size_t leading_bytes, std::size_t search_offset, std::size_t& request_line_end) noexcept {
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

std::optional<http_parse_error> parse_http_header_block(std::string_view buffer, std::size_t leading_bytes, std::size_t header_bytes, parsed_request_header_block& block) {
    const auto headers_end = header_bytes - 2;
    auto cursor_value = leading_bytes;
    if (const auto error = parse_request_line(buffer, headers_end, cursor_value, block)) {
        return error;
    }
    return parse_header_fields(buffer, headers_end, cursor_value, block.protocol_version_ == http_protocol_version::http10, block);
}

}  // namespace ruvia::detail

namespace ruvia {

http_protocol_error http_parse_protocol_error(http_parse_error error) noexcept {
    switch (error) {
        case http_parse_error::header_too_large:
            return http_protocol_error(http_status::request_header_fields_too_large, "header_too_large", "request header is too large");
        case http_parse_error::uri_too_long:
            return http_protocol_error(http_status::uri_too_long, "uri_too_long", "request target is too long");
        case http_parse_error::invalid_request_line:
            return http_protocol_error(http_status::bad_request, "invalid_request_line", "invalid request line");
        case http_parse_error::unsupported_http_version:
            return http_protocol_error(http_status::http_version_not_supported, "unsupported_http_version", "unsupported HTTP version");
        case http_parse_error::invalid_request_target:
            return http_protocol_error(http_status::bad_request, "invalid_request_target", "invalid request target");
        case http_parse_error::invalid_header:
            return http_protocol_error(http_status::bad_request, "invalid_header", "invalid request header");
        case http_parse_error::invalid_connection:
            return http_protocol_error(http_status::bad_request, "invalid_connection", "invalid Connection header");
        case http_parse_error::invalid_upgrade:
            return http_protocol_error(http_status::bad_request, "invalid_upgrade", "invalid Upgrade header");
        case http_parse_error::missing_host:
            return http_protocol_error(http_status::bad_request, "missing_host", "missing Host header");
        case http_parse_error::invalid_host:
            return http_protocol_error(http_status::bad_request, "invalid_host", "invalid Host header");
        case http_parse_error::invalid_content_length:
            return http_protocol_error(http_status::bad_request, "invalid_content_length", "invalid Content-Length header");
        case http_parse_error::conflicting_content_length:
            return http_protocol_error(http_status::bad_request, "conflicting_content_length", "conflicting Content-Length header");
        case http_parse_error::invalid_transfer_encoding:
            return http_protocol_error(http_status::bad_request, "invalid_transfer_encoding", "invalid Transfer-Encoding header");
        case http_parse_error::unsupported_transfer_encoding:
            return http_protocol_error(http_status::not_implemented, "unsupported_transfer_encoding", "unsupported transfer encoding");
    }
    return http_protocol_error(http_status::bad_request, "invalid_request", "invalid HTTP request");
}

}  // namespace ruvia

namespace ruvia::detail {

namespace {

inline constexpr std::string_view chunked_transfer_encoding_header = "Transfer-Encoding: chunked\r\n";

inline constexpr std::string_view content_length_name = "Content-Length";

// Every field line was validated when it was inserted into the response
// (name, value, per-field grammar, forbidden fields, singletons). Only the
// rules that span several lines or depend on this head's framing remain.
[[nodiscard]] bool is_valid_http1_emitted_response_header(std::string_view name, std::string_view value, bool trailer_header_allowed, http_connection_options& connection_options, http_upgrade_protocols& upgrade_protocols) noexcept {
    const auto kind = classify_http_field(name);
    if (kind == http_field_kind::trailer) {
        return trailer_header_allowed || http_trim_ows(value).empty();
    }
    return parse_http_response_control_field(kind, value, connection_options, upgrade_protocols) == http_response_control_field_status::ok;
}

void emit_response_head(const http_response& response, http_raw_wire_sink& sink_value, http_status_code response_status, std::string_view reason_phrase, std::string_view date_header, bool emit_chunked_transfer_encoding, std::string_view content_length_value) noexcept {
    emit_http1_status_line(sink_value, response_status, reason_phrase);

    for (const auto& header : response.headers()) {
        const auto known_bit = http_response_header_access::known_bit(header);
        // HTTP/1 framing belongs exclusively to http1_response_head_plan: the
        // handler's Content-Length and Transfer-Encoding (only ever the
        // replaceable chunked hint) yield to the plan's own framing fields.
        if (known_bit == response_header_transfer_encoding || known_bit == response_header_content_length) {
            continue;
        }
        emit_http1_field_line(sink_value, header.name(), header.value());
    }

    const auto known_bits = response_known_header_bits(response);
    if ((known_bits & response_header_date) == 0 && !date_header.empty()) {
        sink_value.append(date_header);
    }
    if (emit_chunked_transfer_encoding) {
        sink_value.append(chunked_transfer_encoding_header);
    }
    if (!content_length_value.empty()) {
        sink_value.append(content_length_name);
        sink_value.append(std::string_view(": "));
        sink_value.append(content_length_value);
        sink_value.append(std::string_view("\r\n"));
    }
    sink_value.append(std::string_view("\r\n"));
}

template <typename sink_type>
void emit_response_trailer_fields(sink_type& sink, std::span<const http_header_view> fields) {
    for (const auto& field : fields) {
        emit_http1_field_line(sink, field.name(), field.value());
    }
}

// Consume aliased names, values and descriptors before append can reallocate.
#if defined(_MSC_VER)
__declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
void append_response_trailer_snapshot(std::string& output, std::span<const http_header_view> fields, std::size_t bytes) {
    std::array<char, max_http_header_bytes> scratch;
    http_raw_wire_sink sink(scratch.data());
    emit_response_trailer_fields(sink, fields);
    check_http_wire_end(sink.position(), scratch.data() + bytes);
    output.append(scratch.data(), bytes);
}

}  // namespace

}  // namespace ruvia::detail

namespace ruvia {

void append_http1_response_head(const http_response& response, http_response_head_buffer& head, const http1_response_head_plan& plan) {
    const auto body_plan = plan.body_plan();
    const auto* buffered = plan.buffered();
    const auto content_length_plan = buffered != nullptr ? detail::http_framed_buffered_response_head_plan(body_plan, buffered->content_length(), response) : detail::http_framed_streaming_response_head_plan(body_plan, response);
    const auto* framed = std::get_if<detail::http_framed_response_head_plan>(&content_length_plan);
    if (framed == nullptr) {
        switch (std::get<detail::http_framed_response_head_plan_error>(content_length_plan)) {
            case detail::http_framed_response_head_plan_error::invalid_content_length:
                throw http1_response_head_error("invalid explicit HTTP response Content-Length");
            case detail::http_framed_response_head_plan_error::response_status_mismatch:
                throw std::invalid_argument("HTTP/1 response plan status does not match response");
            case detail::http_framed_response_head_plan_error::response_representation_mismatch:
                throw std::invalid_argument("HTTP/1 response plan representation does not match response");
            case detail::http_framed_response_head_plan_error::connect_tunnel_required:
                throw http1_response_head_error("HTTP CONNECT tunnel response required");
        }
    }
    if (!detail::response_transfer_encoding_replaceable(response)) {
        throw http1_response_head_error("HTTP response Transfer-Encoding must be exactly chunked");
    }
    const auto response_status = body_plan.response_status();
    const bool chunked_payload_plan = plan.chunked_stream() != nullptr && body_plan.transfer_encoding_allowed() && !body_plan.body_suppressed();
    if (chunked_payload_plan && plan.protocol_version() == http_protocol_version::http10) {
        throw http1_response_head_error("HTTP/1.0 response cannot use chunked Transfer-Encoding");
    }
    const bool emit_chunked_transfer_encoding = chunked_payload_plan;
    // HTTP/1 alone can frame a stream with chunked coding or connection close.
    // Those delimiters replace Content-Length except suppressed HEAD metadata.
    const auto content_length = emit_chunked_transfer_encoding || (plan.close_delimited_stream() != nullptr && !body_plan.body_suppressed()) ? std::nullopt : framed->content_length();
    const auto known_bits = detail::response_known_header_bits(response);

    const auto reason_phrase = http_reason_phrase(response_status);
    const auto date_header = (known_bits & detail::response_header_date) == 0 ? detail::cached_date_header() : std::string_view{};
    // Reuse the formatted token for exact measurement and emission instead of
    // separately dividing the length to count its digits.
    // Buffered HEAD metadata retains the selected representation length.
    // A status-level no-content policy that still owns framing (205) is
    // canonicalized to zero for both buffered and streaming heads.
    std::array<char, http_max_unsigned_decimal_size> content_length_buffer;
    const auto content_length_value = content_length.has_value() ? http_format_unsigned(content_length_buffer, *content_length) : std::string_view{};

    // Measure the exact emitted head before touching reusable output storage.
    // This both bounds the unchecked raw stack sink and enforces the shared
    // field-section rule used by request parsing.
    http_field_section_budget head_budget;
    const auto charge = [](bool fits) {
        if (!fits) {
            throw http1_response_head_error("HTTP response head is too large");
        }
    };
    charge(head_budget.add(detail::http1_status_line_size(reason_phrase)));
    detail::http_connection_options connection_options;
    detail::http_upgrade_protocols upgrade_protocols;
    for (const auto& header : response.headers()) {
        const auto known_bit = detail::http_response_header_access::known_bit(header);
        if (known_bit == detail::response_header_transfer_encoding || known_bit == detail::response_header_content_length) {
            continue;
        }
        if (!detail::is_valid_http1_emitted_response_header(header.name(), header.value(), emit_chunked_transfer_encoding, connection_options, upgrade_protocols)) {
            throw http1_response_head_error("invalid HTTP response header");
        }
        charge(head_budget.add_field(header.name(), header.value()));
    }
    if (!detail::http_response_upgrade_pair_valid(connection_options, upgrade_protocols)) {
        throw http1_response_head_error("HTTP Upgrade response requires Connection: Upgrade");
    }
    if ((known_bits & detail::response_header_date) == 0 && !date_header.empty()) {
        charge(head_budget.add(date_header.size()));
    }
    if (emit_chunked_transfer_encoding) {
        charge(head_budget.add(detail::chunked_transfer_encoding_header.size()));
    }
    if (!content_length_value.empty()) {
        charge(head_budget.add_field(detail::content_length_name.size(), content_length_value.size()));
    }
    charge(head_budget.add(2));

    head.append_generated(head_budget.bytes(), [&](char* cursor_value) noexcept {
        detail::http_raw_wire_sink sink_value(cursor_value);
        detail::emit_response_head(response, sink_value, response_status, reason_phrase, date_header, emit_chunked_transfer_encoding, content_length_value);
        detail::check_http_wire_end(sink_value.position(), cursor_value + head_budget.bytes());
    });
}

void append_http1_response_trailers(std::string& output, const http_response_trailer_section& trailers) {
    const auto fields = trailers.fields();
    if (fields.empty()) {
        return;
    }
    auto budget = http_trailer_section_budget();
    for (const auto& field : fields) {
        if (!budget.add_field(field.name(), field.value())) {
            throw std::length_error("HTTP response trailers are too large");
        }
    }
    const auto bytes = budget.bytes() - std::string_view("\r\n").size();
    if (bytes > output.max_size() - output.size()) {
        throw std::length_error("HTTP response trailers are too large");
    }
    if (detail::http_wire_inputs_overlap(output, fields)) {
        detail::append_response_trailer_snapshot(output, fields, bytes);
        return;
    }
    output.reserve(output.size() + bytes);
    detail::http_string_wire_sink sink(output);
    detail::emit_response_trailer_fields(sink, fields);
}

namespace {

// Lists one generated option; an application Connection line is extended
// rather than replaced, so its own options survive.
void add_http1_connection_option(http_response& response, std::string_view option, bool field_present) {
    if (field_present) {
        response.header(detail::http_connection_field_name, option, http_response_header_options{.mode_ = http_response_header_mode::append});
    } else {
        response.header_stable_view(detail::http_connection_field_name, option);
    }
}

// Visits each protocol the Upgrade fields of `request` offer, in field order,
// until `visitor` returns false. Malformed members end the visit.
template <typename visitor_type>
void visit_offered_upgrade_protocols(const http_request& request, visitor_type&& visitor) noexcept {
    const auto headers = request.headers();
    bool visiting = true;
    for (std::size_t index = 0; visiting && index < headers.size(); ++index) {
        if (detail::http_request_access::header_kind(request, index) != detail::http_field_kind::upgrade) {
            continue;
        }
        (void)detail::http_upgrade_protocols{}.parse_field(headers[index].value(), detail::http_field_list_role::recipient, [&visiting, &visitor](const detail::http_upgrade_protocol& protocol) noexcept {
            visiting = visitor(protocol);
            return visiting;
        });
    }
}

[[nodiscard]] bool request_offers_upgrade_protocol(const http_request& request, const detail::http_upgrade_protocol& selected) noexcept {
    bool offered = false;
    visit_offered_upgrade_protocols(request, [&offered, &selected](const detail::http_upgrade_protocol& protocol) noexcept {
        offered = detail::http_upgrade_protocol_equals(protocol, selected);
        return !offered;
    });
    return offered;
}

}  // namespace

http1_request_connection_plan http1_commit_final_response(http_response& response, http1_request_connection_plan plan) {
    const auto http1_control = detail::http1_final_response_control_plan(response);
    const auto response_options = http1_control.connection_options();
    const auto upgrade_protocols = http1_control.upgrade_protocols();
    const bool preserve_upgrade = upgrade_protocols.has_field();
    // A failed CONNECT never becomes a tunnel. RFC 9931 section 8 requires
    // closing instead of interpreting optimistic tunnel data as more requests.
    if (response_options.close() || (plan.tunnel_request() && !response.status().is_successful())) {
        plan = plan.require_close();
    }
    if (plan.disposition() == http1_close_policy::close_after_response) {
        http1_mark_connection_close(response, preserve_upgrade ? http1_connection_close_field_policy::preserve_upgrade : http1_connection_close_field_policy::close_only);
    } else {
        const bool generate_upgrade_option = preserve_upgrade && !response_options.upgrade();
        if (generate_upgrade_option) {
            add_http1_connection_option(response, detail::http_connection_upgrade_option, response_options.has_field());
        }
        if (plan.protocol_version() == http_protocol_version::http10 && !response_options.keep_alive()) {
            add_http1_connection_option(response, detail::http_connection_keep_alive_option, response_options.has_field() || generate_upgrade_option);
        }
    }
    return plan;
}

prepared_http1_response_stream prepare_http1_response_stream_head(http_response response, const http1_response_stream_plan& plan, http_response_trailer_intent trailer_intent) {
    auto commit_plan = plan_http_response_stream_commit(plan.framing(), plan.request_method(), response.status(), trailer_intent);
    const auto body_plan = commit_plan.body_plan();
    // HTTP/1.0 cannot delimit an open-ended response stream without closing the
    // connection, but a response whose method/status forbids payload is already
    // self-delimited. Make this decision at head commit, when the response status is
    // finally known, instead of pessimistically baking close into the pre-commit plan.
    const auto planned_connection = plan.request_connection_plan().disposition() == http1_close_policy::allow_reuse && plan.close_policy() == http1_close_policy::allow_reuse && (plan.framing() != http_response_stream_framing::http1_close_delimited || body_plan.body_suppressed()) ? plan.request_connection_plan() : plan.request_connection_plan().require_close();
    const auto connection_plan = http1_commit_final_response(response, planned_connection);
    auto head = prepare_http_response_stream_head(std::move(response), std::move(commit_plan));
    const auto response_head_plan = plan.framing() == http_response_stream_framing::http1_chunked ? http1_chunked_response_stream_head_plan(head.commit_plan().body_plan(), connection_plan) : http1_close_delimited_response_stream_head_plan(head.commit_plan().body_plan(), connection_plan);
    return prepared_http1_response_stream(std::move(head), response_head_plan, connection_plan);
}

bool http1_request_offers_upgrade(const http_request& request) noexcept {
    if (request.protocol_version() != http_protocol_version::http11 || request.known_method() == http_known_method::connect) {
        return false;
    }
    detail::http_connection_options connection_options;
    detail::http_upgrade_protocols upgrade_protocols;
    const auto headers = request.headers();
    for (std::size_t index = 0; index < headers.size(); ++index) {
        const auto kind = detail::http_request_access::header_kind(request, index);
        if (kind == detail::http_field_kind::connection) {
            if (connection_options.parse_field(headers[index].value(), detail::http_connection_field_role::request_recipient) != detail::http_field_list_parse_status::ok) {
                return false;
            }
        } else if (kind == detail::http_field_kind::upgrade) {
            if (upgrade_protocols.parse_field(headers[index].value(), detail::http_field_list_role::recipient) != detail::http_field_list_parse_status::ok) {
                return false;
            }
        }
    }
    return connection_options.upgrade() && upgrade_protocols.has_protocol();
}

http1_response_head_plan prepare_http1_upgrade_response_head(http_response& response, const http_request& request) {
    if (!http1_request_offers_upgrade(request)) {
        throw http1_response_head_error("HTTP/1 request offers no protocol upgrade");
    }
    if (response.file_body() || response.has_multipart_file_body() || !response.body_bytes().empty() || detail::response_has_known_header(response, detail::response_header_content_length | detail::response_header_transfer_encoding)) {
        throw http1_response_head_error("HTTP/1 Switching Protocols response cannot carry content or framing fields");
    }
    const auto http1_control = detail::http1_final_response_control_plan(response);
    // RFC 9110 §7.8: the server switches only to protocols the client offered.
    if (http1_control.upgrade_protocols().has_field()) {
        bool offered = http1_control.upgrade_protocols().has_protocol();
        for (const auto& header : response.headers()) {
            if (offered && detail::http_response_header_access::known_bit(header) == detail::response_header_upgrade) {
                (void)detail::http_upgrade_protocols{}.parse_field(header.value(), detail::http_field_list_role::sender, [&offered, &request](const detail::http_upgrade_protocol& selected) noexcept {
                    offered = request_offers_upgrade_protocol(request, selected);
                    return offered;
                });
            }
        }
        if (!offered) {
            throw http1_response_head_error("HTTP/1 Switching Protocols response names a protocol the request did not offer");
        }
    } else {
        std::string_view offered_protocol;
        visit_offered_upgrade_protocols(request, [&offered_protocol](const detail::http_upgrade_protocol& protocol) noexcept {
            const auto end = protocol.version_.empty() ? protocol.name_.data() + protocol.name_.size() : protocol.version_.data() + protocol.version_.size();
            offered_protocol = std::string_view(protocol.name_.data(), static_cast<std::size_t>(end - protocol.name_.data()));
            return false;
        });
        response.header("Upgrade", offered_protocol);
    }
    if (!http1_control.connection_options().upgrade()) {
        add_http1_connection_option(response, detail::http_connection_upgrade_option, http1_control.connection_options().has_field());
    }
    response.status_code_ = http_status::switching_protocols;
    return http1_close_delimited_response_stream_head_plan(plan_http_response_body(http_known_method::get, http_status::switching_protocols), http1_request_connection_plan::http11_close());
}

}  // namespace ruvia

namespace ruvia {

http_response_head_buffer::http_response_head_buffer(const http_response_head_buffer& other) :
    heap_(other.heap_) {}

http_response_head_buffer::http_response_head_buffer(http_response_head_buffer&& other) noexcept :
    heap_(std::move(other.heap_)) {
    copy_committed_state(other);
}

http_response_head_buffer& http_response_head_buffer::operator=(const http_response_head_buffer& other) {
    if (this != &other) {
        if (other.heap_.size() > heap_.capacity()) {
            // Retire the old allocation only after the complete state is published.
            std::string prepared(other.heap_);
            heap_.swap(prepared);
            copy_committed_state(other);
            return *this;
        }
        heap_ = other.heap_;
        copy_committed_state(other);
    }
    return *this;
}

http_response_head_buffer& http_response_head_buffer::operator=(http_response_head_buffer&& other) noexcept {
    if (this != &other) {
        heap_ = std::move(other.heap_);
        copy_committed_state(other);
    }
    return *this;
}

void http_response_head_buffer::copy_committed_state(const http_response_head_buffer& other) noexcept {
    // Heap replacement must succeed before publishing the inline bytes and state.
    if (const auto* const stack_state = std::get_if<stack_state_type>(&other.state_)) {
        std::memcpy(stack_.data(), other.stack_.data(), stack_state->used_);
    }
    state_ = other.state_;
}

void http_response_head_buffer::reset() noexcept {
    // Publish the empty inline state before retiring storage can reenter this buffer.
    state_.emplace<stack_state_type>();
    http_clear_string_retaining_small(heap_, response_head_retained_heap_bytes);
}

void http_response_head_buffer::spill_to_heap(std::size_t min_capacity) {
    heap_.reserve(std::max(stack_.size() * 2, min_capacity));
    heap_.assign(stack_.data(), std::get<stack_state_type>(state_).used_);
    state_.emplace<heap_state_type>();
}

void http_response_head_buffer::append(std::string_view value) {
    if (value.empty()) {
        return;
    }
    if (auto* const stack_state = std::get_if<stack_state_type>(&state_)) {
        if (value.size() <= stack_.size() - stack_state->used_) {
            std::memmove(stack_.data() + stack_state->used_, value.data(), value.size());
            stack_state->used_ += value.size();
            return;
        }
        if (value.size() > heap_.max_size() - stack_state->used_) {
            throw std::length_error("HTTP response head is too large");
        }
        spill_to_heap(stack_state->used_ + value.size());
        heap_.append(value);
        return;
    }
    if (value.size() > heap_.max_size() - heap_.size()) {
        throw std::length_error("HTTP response head is too large");
    }
    if (detail::http_text_aliases_string(value, heap_)) {
        std::string copy(value);
        heap_.append(copy);
        return;
    }
    heap_.append(value);
}

void http_response_head_buffer::append(char value) {
    if (auto* const stack_state = std::get_if<stack_state_type>(&state_)) {
        if (stack_state->used_ < stack_.size()) {
            stack_[stack_state->used_++] = value;
            return;
        }
        spill_to_heap(stack_state->used_ + 1);
    }
    heap_.push_back(value);
}

void http_response_head_buffer::append_unsigned(std::uint64_t value) {
    std::array<char, http_max_unsigned_decimal_size> digits;
    append(http_format_unsigned(digits, value));
}

void http_response_head_buffer::reserve_additional(std::size_t size) {
    if (std::holds_alternative<heap_state_type>(state_)) {
        if (size > heap_.max_size() - heap_.size()) {
            throw std::length_error("HTTP response head is too large");
        }
        heap_.reserve(heap_.size() + size);
        return;
    }
    const auto used = std::get<stack_state_type>(state_).used_;
    if (size <= stack_.size() - used) {
        return;
    }
    if (size > heap_.max_size() - used) {
        throw std::length_error("HTTP response head is too large");
    }
    spill_to_heap(used + size);
}

std::string_view http_response_head_buffer::view() const& noexcept {
    if (const auto* const stack_state = std::get_if<stack_state_type>(&state_)) {
        return std::string_view(stack_.data(), stack_state->used_);
    }
    return std::string_view(heap_);
}

bool http_response_head_buffer::can_append_on_stack(std::size_t size) const noexcept {
    const auto* const stack_state = std::get_if<stack_state_type>(&state_);
    return stack_state != nullptr && size <= stack_.size() - stack_state->used_;
}

}  // namespace ruvia

namespace ruvia {

namespace {

struct http_chunk_size_line final {
    std::size_t size_{0};
    // Line bytes beyond the shortest hex spelling of size_: leading zeros and
    // chunk-ext. A decoder charges them to a per-message framing budget.
    std::size_t overhead_bytes_{0};
};

bool is_valid_http_chunk_extension(std::string_view value) noexcept {
    if (value.empty()) {
        return true;
    }

    std::size_t cursor_value = 0;
    const auto skip_bws = [&value, &cursor_value]() noexcept {
        while (cursor_value < value.size() && is_http_ows_char(value[cursor_value])) {
            ++cursor_value;
        }
    };
    const auto parse_token = [&value, &cursor_value]() noexcept {
        const auto begin = cursor_value;
        while (cursor_value < value.size() && is_http_token_char(static_cast<unsigned char>(value[cursor_value]))) {
            ++cursor_value;
        }
        return cursor_value != begin;
    };

    while (cursor_value < value.size()) {
        skip_bws();
        if (cursor_value == value.size()) {
            // Reached end-of-line after consuming whitespace with no ";": the
            // chunk-ext grammar (RFC 9112 7.1) permits BWS only before ";"/"=",
            // never as trailing space before CRLF. Accepting "5 " is a
            // request-smuggling differential of the same class as the leading-OWS
            // case that parse_http_chunk_size_line already rejects.
            return false;
        }
        if (value[cursor_value] != ';') {
            return false;
        }
        ++cursor_value;
        skip_bws();
        if (!parse_token()) {
            return false;
        }
        const auto after_name = cursor_value;
        skip_bws();
        if (cursor_value == value.size()) {
            // Bare ext-name at end-of-line: valid only with no trailing BWS
            // between the name and the CRLF (RFC 9112 7.1).
            return cursor_value == after_name;
        }
        if (value[cursor_value] == ';') {
            continue;
        }
        if (value[cursor_value] != '=') {
            return false;
        }
        ++cursor_value;
        skip_bws();
        if (cursor_value == value.size()) {
            return false;
        }
        if (value[cursor_value] == '"') {
            const auto quoted_size = http_quoted_string_size(value.substr(cursor_value));
            if (quoted_size == 0) {
                return false;
            }
            cursor_value += quoted_size;
        } else if (!parse_token()) {
            return false;
        }
        const auto before_trailing_bws = cursor_value;
        skip_bws();
        if (cursor_value == value.size()) {
            // End-of-line after a chunk-ext value is valid only if it lands
            // exactly on the value; whitespace between the value and CRLF is
            // rejected for the same reason as trailing space after the size.
            return cursor_value == before_trailing_bws;
        }
        if (value[cursor_value] != ';') {
            return false;
        }
    }
    return true;
}

// Parses `chunk-size [ chunk-ext ]` (RFC 9112 7.1) without its CRLF; nullopt on
// any malformed or overflowing line.
std::optional<http_chunk_size_line> parse_http_chunk_size_line(std::string_view value) noexcept {
    // RFC 9112: chunk-size starts at the first byte of the line. Leading OWS
    // before the size is a known request-smuggling vector and is rejected,
    // matching picohttpparser/llhttp strict parsing.
    std::size_t cursor_value = 0;
    std::size_t leading_zero_digits = 0;
    std::size_t parsed_value = 0;
    constexpr auto max_before_shift = std::numeric_limits<std::size_t>::max() >> 4U;
    while (cursor_value < value.size()) {
        const int nibble = http_decode_hex_nibble(value[cursor_value]);
        if (nibble < 0) {
            break;
        }
        if (parsed_value > max_before_shift) {
            return std::nullopt;
        }
        if (parsed_value == 0 && nibble == 0) {
            ++leading_zero_digits;
        }
        parsed_value = (parsed_value << 4U) | static_cast<std::size_t>(nibble);
        ++cursor_value;
    }
    if (cursor_value == 0 || !is_valid_http_chunk_extension(value.substr(cursor_value))) {
        return std::nullopt;
    }

    // The shortest spelling of a size drops its leading zeros but keeps one
    // digit for a zero size.
    const auto significant_digits = std::max<std::size_t>(cursor_value - leading_zero_digits, 1);
    return http_chunk_size_line{.size_ = parsed_value, .overhead_bytes_ = value.size() - significant_digits};
}

// The HTTP/1 received trailer-block field parser. `block` holds the field
// lines between the last chunk and the empty line that ends the trailer
// section. Values are exposed with field-line OWS trimmed.
// `obs_fold` decides whether a line starting with OWS continues the previous
// field (RFC 9112 section 5.2). Rejected, such a line is malformed. Accepted,
// the field is validated and charged as its HTTP unfolding (each obs-fold and
// adjacent OWS is one SP), and the visitor receives the received value span
// with its folds in place. Every field is charged to the section budget;
// syntax errors and the budget fail the whole section. The visitor receives
// views borrowing `block`.
template <typename visitor_type>
[[nodiscard]] bool visit_http1_trailer_block(std::string_view block, http1_trailer_obs_fold obs_fold, visitor_type&& visitor) {
    auto budget = http_trailer_section_budget();
    const auto line_at = [block](std::size_t begin, std::size_t end) noexcept {
        return end == std::string_view::npos ? block.substr(begin) : block.substr(begin, end - begin);
    };
    std::size_t cursor_value = 0;
    while (cursor_value < block.size()) {
        auto line_end = block.find("\r\n", cursor_value);
        const auto line = line_at(cursor_value, line_end);
        // Finding the separator also proves that the complete name is a token.
        const auto colon = http_token_prefix_size(line);
        if (colon == 0 || colon == line.size() || line[colon] != ':') {
            return false;
        }
        const auto name = line.substr(0, colon);
        // The unfolded value spans the first to the last non-empty piece, each
        // fold between them counting as one SP.
        auto piece = http_trim_ows(line.substr(colon + 1));
        auto value = piece;
        std::size_t value_bytes = 0;
        std::size_t pending_separators = 0;
        for (;;) {
            if (!is_valid_http_field_value_bytes(piece)) {
                return false;
            }
            if (!piece.empty()) {
                if (value_bytes == 0) {
                    value = piece;
                } else {
                    value_bytes += pending_separators;
                    value = std::string_view(value.data(), static_cast<std::size_t>(piece.data() - value.data()) + piece.size());
                }
                value_bytes += piece.size();
                pending_separators = 0;
            }
            if (obs_fold == http1_trailer_obs_fold::reject || line_end == std::string_view::npos || line_end + 2 >= block.size() || !is_http_ows_char(block[line_end + 2])) {
                break;
            }
            ++pending_separators;
            const auto piece_begin = line_end + 2;
            line_end = block.find("\r\n", piece_begin);
            piece = http_trim_ows(line_at(piece_begin, line_end));
        }
        if (!budget.add_field(name.size(), value_bytes)) {
            return false;
        }
        visitor(name, value);
        if (line_end == std::string_view::npos) {
            break;
        }
        cursor_value = line_end + 2;
    }
    return true;
}

}  // namespace

http1_chunk_decode_result http1_chunk_decode_result::make_need_more(std::size_t consumed_bytes) noexcept {
    return http1_chunk_decode_result(http1_chunk_decode_need_more(consumed_bytes));
}

http1_chunk_decode_result http1_chunk_decode_result::make_body_chunk(std::size_t consumed_bytes, std::string_view bytes_value) noexcept {
    return http1_chunk_decode_result(http1_chunk_decode_body_chunk_view(consumed_bytes, bytes_value));
}

http1_chunk_decode_result http1_chunk_decode_result::make_complete(std::size_t consumed_bytes, std::span<const http_header> trailers) noexcept {
    return http1_chunk_decode_result(http1_chunk_decode_complete_view(consumed_bytes, trailers));
}

http1_chunk_decode_result http1_chunk_decode_result::make_failure(std::size_t consumed_bytes, http1_chunk_decode_error error) noexcept {
    return http1_chunk_decode_result(http1_chunk_decode_failure(consumed_bytes, error));
}

http1_chunked_body_decoder::http1_chunked_body_decoder(http1_chunked_body_decoder_config config) :
    body_limit_(config.body_limit_),
    obs_fold_(config.obs_fold_),
    trailer_message_(config.trailer_message_) {}

http1_chunked_body_decoder::~http1_chunked_body_decoder() = default;
http1_chunked_body_decoder::http1_chunked_body_decoder(http1_chunked_body_decoder&&) noexcept = default;
http1_chunked_body_decoder& http1_chunked_body_decoder::operator=(http1_chunked_body_decoder&&) noexcept = default;

http1_chunk_decode_result http1_chunked_body_decoder::decode(std::string_view available) {
    return decode(available, std::numeric_limits<std::size_t>::max());
}

http1_chunk_decode_result http1_chunked_body_decoder::decode(std::string_view available, std::size_t max_body_bytes) {
    if (max_body_bytes == 0) {
        throw std::invalid_argument("HTTP/1 chunk decode body quota must be greater than zero");
    }
    if (const auto* error = std::get_if<http1_chunk_decode_error>(&state_)) {
        return http1_chunk_decode_result::make_failure(0, *error);
    }
    // Published trailers live until this next call.
    trailers_.clear();

    std::size_t cursor = 0;
    for (;;) {
        switch (std::get<progress>(state_)) {
            case progress::size_line: {
                // Every LF must end a CRLF (RFC 9112 section 7.1), so a bare LF is
                // rejected as soon as it arrives instead of waiting for a CRLF.
                const auto line = available.substr(cursor, max_http_header_bytes);
                const auto line_feed = line.find('\n', size_line_search_offset_);
                if (line_feed == std::string_view::npos) {
                    const auto line_bytes = line.size();
                    if (line_bytes >= max_http_header_bytes) {
                        return fail(cursor, http1_chunk_decode_error::framing_limit_exceeded);
                    }
                    size_line_search_offset_ = line_bytes;
                    return http1_chunk_decode_result::make_need_more(cursor);
                }
                if (line_feed == 0 || line[line_feed - 1] != '\r') {
                    return fail(cursor, http1_chunk_decode_error::invalid_framing);
                }
                size_line_search_offset_ = 0;
                const auto line_end = line_feed - 1;
                const auto size_line = parse_http_chunk_size_line(line.substr(0, line_end));
                if (!size_line) {
                    return fail(cursor, http1_chunk_decode_error::invalid_framing);
                }
                // RFC 9112 section 7.1.1: a recipient ought to limit the total
                // chunk-ext length of a message; leading zeros cost the same scan.
                if (max_http_header_bytes - framing_overhead_bytes_ < size_line->overhead_bytes_) {
                    return fail(cursor, http1_chunk_decode_error::framing_limit_exceeded);
                }
                framing_overhead_bytes_ += size_line->overhead_bytes_;
                cursor += line_end + 2;
                if (size_line->size_ == 0) {
                    state_ = progress::trailers;
                    trailer_search_offset_ = 0;
                } else {
                    if (body_limit_.addition_exceeds(decoded_bytes_, size_line->size_)) {
                        return fail(cursor, http1_chunk_decode_error::body_limit_exceeded);
                    }
                    decoded_bytes_ += size_line->size_;
                    remaining_ = size_line->size_;
                    state_ = progress::body;
                }
                break;
            }
            case progress::body: {
                const auto bytes_value = std::min({remaining_, available.size() - cursor, max_body_bytes});
                if (bytes_value == 0) {
                    return http1_chunk_decode_result::make_need_more(cursor);
                }
                const auto body = available.substr(cursor, bytes_value);
                remaining_ -= bytes_value;
                cursor += bytes_value;
                if (remaining_ == 0) {
                    state_ = progress::delimiter;
                    // A buffered delimiter is consumed with the final data bytes,
                    // so consumed_bytes covers the whole chunk.
                    if (available.size() - cursor >= 2 && !consume_delimiter(available, cursor)) {
                        return fail(cursor, http1_chunk_decode_error::invalid_framing);
                    }
                }
                return http1_chunk_decode_result::make_body_chunk(cursor, body);
            }
            case progress::delimiter:
                if (available.size() - cursor < 2) {
                    return http1_chunk_decode_result::make_need_more(cursor);
                }
                if (!consume_delimiter(available, cursor)) {
                    return fail(cursor, http1_chunk_decode_error::invalid_framing);
                }
                break;
            case progress::trailers: {
                const auto trailers = available.substr(cursor, max_http_header_bytes);
                if (trailers.starts_with("\r\n")) {
                    state_ = progress::complete;
                    return http1_chunk_decode_result::make_complete(cursor + 2);
                }
                // Scan only bytes not examined by an earlier call. Each LF must be
                // preceded by CR; an LF directly after a CRLF ends the section.
                auto trailer_end = std::string_view::npos;
                for (auto line_feed = trailers.find('\n', trailer_search_offset_); line_feed != std::string_view::npos; line_feed = trailers.find('\n', line_feed + 1)) {
                    if (line_feed == 0 || trailers[line_feed - 1] != '\r') {
                        return fail(cursor, http1_chunk_decode_error::invalid_framing);
                    }
                    if (line_feed >= 3 && trailers[line_feed - 2] == '\n') {
                        trailer_end = line_feed - 3;
                        break;
                    }
                }
                if (trailer_end == std::string_view::npos) {
                    if (trailers.size() >= max_http_header_bytes) {
                        return fail(cursor, http1_chunk_decode_error::framing_limit_exceeded);
                    }
                    trailer_search_offset_ = trailers.size();
                    return http1_chunk_decode_result::make_need_more(cursor);
                }
                const auto trailer_bytes = trailer_end + 4;
                if (!publish_trailers(trailers.substr(0, trailer_end))) {
                    return fail(cursor, http1_chunk_decode_error::invalid_framing);
                }
                state_ = progress::complete;
                return http1_chunk_decode_result::make_complete(cursor + trailer_bytes, trailers_);
            }
            case progress::complete:
                return http1_chunk_decode_result::make_complete(cursor);
        }
    }
}

// Requires both delimiter bytes to be buffered at cursor.
bool http1_chunked_body_decoder::consume_delimiter(std::string_view available, std::size_t& cursor) noexcept {
    if (!available.substr(cursor).starts_with("\r\n")) {
        return false;
    }
    cursor += 2;
    state_ = progress::size_line;
    return true;
}

bool http1_chunked_body_decoder::publish_trailers(std::string_view trailers) {
    // Received fields that were not permitted as trailers are discarded, never
    // merged (RFC 9112 section 7.1.2), so framing validity depends only on
    // field syntax and the section budget.
    const auto discarded = [message = trailer_message_](std::string_view name) noexcept {
        return message == http1_trailer_message::request ? detail::is_forbidden_http_request_trailer_name(name) : detail::is_forbidden_response_trailer_name(name);
    };
    const auto publish = [this, &discarded](std::string_view name, std::string_view value) {
        if (discarded(name)) {
            return;
        }
        // RFC 9112 section 5.2: each accepted obs-fold becomes SP before the
        // value is interpreted.
        if (const auto fold = detail::find_field_line_fold(value); fold != std::string_view::npos) {
            std::string unfolded;
            detail::copy_unfolded_field_lines(value, fold, detail::field_line_folding_policy::http, unfolded);
            trailers_.push_back(http_header::copy_with_lowercase_name(name, unfolded));
            return;
        }
        trailers_.push_back(http_header::copy_with_lowercase_name(name, value));
    };
    bool valid = false;
    try {
        valid = visit_http1_trailer_block(trailers, obs_fold_, publish);
    } catch (...) {
        trailers_.clear();
        throw;
    }
    if (!valid) {
        trailers_.clear();
    }
    return valid;
}

http1_chunk_decode_result http1_chunked_body_decoder::fail(std::size_t consumed_bytes, http1_chunk_decode_error error) noexcept {
    state_ = error;
    return http1_chunk_decode_result::make_failure(consumed_bytes, error);
}

}  // namespace ruvia

namespace ruvia {

namespace detail {

// One zlib stage is an implementation detail of the coding sequence, not a
// second public decoder. Its address must stay fixed for the pinned zlib stream.
class transfer_coding_decoder final {
public:
    transfer_coding_decoder(http_transfer_coding coding, protocol_byte_limit decoded_limit);
    transfer_coding_decoder(const transfer_coding_decoder&) = delete;
    transfer_coding_decoder& operator=(const transfer_coding_decoder&) = delete;
    [[nodiscard]] http_transfer_coding_decode_result decode(std::string_view input, std::span<char> output) noexcept;
    [[nodiscard]] http_transfer_coding_decode_result finish_input() noexcept;
    [[nodiscard]] bool received_input() const noexcept {
        return received_input_;
    }

private:
    struct inflate_step_result {
        std::size_t consumed_{0};
        std::size_t produced_{0};
        int status_{Z_OK};
    };
    struct active final {};
    struct gzip_member_boundary final {};
    struct completed final {};
    using state = std::variant<active, gzip_member_boundary, completed>;
    [[nodiscard]] inflate_step_result inflate_step(std::string_view input, std::span<char> output) noexcept;
    [[nodiscard]] static http_transfer_coding_decode_result need_input(std::size_t consumed) noexcept;
    [[nodiscard]] static http_transfer_coding_decode_result output(std::size_t consumed, std::string_view bytes) noexcept;
    [[nodiscard]] static http_transfer_coding_decode_result complete(std::size_t consumed) noexcept;
    [[nodiscard]] static http_transfer_coding_decode_result fail(std::size_t consumed, http_transfer_coding_decode_error error) noexcept;
    [[nodiscard]] static http_transfer_coding_decode_result fail_decoder(std::size_t consumed) noexcept;
    zlib_inflate_stream stream_;
    state state_{active{}};
    protocol_byte_limit body_limit_;
    std::size_t decoded_bytes_{0};
    // An empty coded body carries no content and completes at framing EOF.
    bool received_input_{false};
    http_transfer_coding coding_;
};

namespace {

[[nodiscard]] int transfer_coding_window_bits(http_transfer_coding coding) {
    switch (coding) {
        case http_transfer_coding::gzip:
            return 15 + 16;
        case http_transfer_coding::deflate:
            return 15;
        default:
            throw std::invalid_argument("unsupported HTTP transfer coding");
    }
}

}  // namespace

transfer_coding_decoder::transfer_coding_decoder(http_transfer_coding coding, protocol_byte_limit decoded_limit) :
    stream_(transfer_coding_window_bits(coding)),
    body_limit_(decoded_limit),
    coding_(coding) {
    if (!stream_.initialized()) {
        throw std::runtime_error("failed to initialize transfer-coding decoder");
    }
}

http_transfer_coding_decode_result transfer_coding_decoder::decode(std::string_view input, std::span<char> output_buffer) noexcept {
    if (std::holds_alternative<completed>(state_)) {
        return input.empty() ? complete(0) : fail(0, http_transfer_coding_decode_error::invalid_content);
    }

    std::size_t consumed = 0;
    std::size_t produced = 0;
    for (;;) {
        if (std::holds_alternative<gzip_member_boundary>(state_)) {
            if (consumed == input.size()) {
                return produced != 0 ? output(consumed, std::string_view(output_buffer.data(), produced)) : need_input(consumed);
            }
            if (!stream_.reset()) {
                return fail_decoder(consumed);
            }
            state_.emplace<active>();
        }

        const auto window_bytes = std::min<std::size_t>(input.size() - consumed, (std::numeric_limits<uInt>::max)());
        const auto step = inflate_step(input.substr(consumed), output_buffer.subspan(produced));
        consumed += step.consumed_;
        received_input_ = received_input_ || step.consumed_ != 0;
        if (body_limit_.addition_exceeds(decoded_bytes_, step.produced_)) {
            return fail(consumed, http_transfer_coding_decode_error::decoded_size_exceeded);
        }
        decoded_bytes_ += step.produced_;
        produced += step.produced_;

        if (step.status_ == Z_STREAM_END) {
            if (coding_ != http_transfer_coding::gzip) {
                if (consumed != input.size()) {
                    return fail(consumed, http_transfer_coding_decode_error::invalid_content);
                }
                state_.emplace<completed>();
                return produced != 0 ? output(consumed, std::string_view(output_buffer.data(), produced)) : complete(consumed);
            }

            // RFC 1952 gzip data is a series of members. A member boundary is
            // not the end of the transfer coding: another member can arrive in
            // the same HTTP chunk or in a later one. Only framing EOF, reported
            // through finish_input(), commits this boundary as complete.
            state_.emplace<gzip_member_boundary>();
            if (produced == output_buffer.size()) {
                return output(consumed, std::string_view(output_buffer.data(), produced));
            }
            continue;
        }

        if (step.status_ != Z_OK && step.status_ != Z_BUF_ERROR) {
            if (step.status_ == Z_DATA_ERROR || step.status_ == Z_NEED_DICT) {
                return fail(consumed, http_transfer_coding_decode_error::invalid_content);
            }
            return fail_decoder(consumed);
        }

        if (produced != 0) {
            return output(consumed, std::string_view(output_buffer.data(), produced));
        }
        if (consumed != input.size()) {
            // zlib sees at most one uInt window per step; a window that was
            // consumed without output (RFC 1952 FNAME/FCOMMENT bytes) is not
            // corrupt, the remaining input needs another step.
            if (step.consumed_ == window_bytes) {
                continue;
            }
            return fail(consumed, http_transfer_coding_decode_error::invalid_content);
        }
        return need_input(consumed);
    }
}

http_transfer_coding_decode_result transfer_coding_decoder::finish_input() noexcept {
    if (std::holds_alternative<completed>(state_) || std::holds_alternative<gzip_member_boundary>(state_) || (std::holds_alternative<active>(state_) && !received_input_)) {
        state_.emplace<completed>();
        return complete(0);
    }
    return fail(0, http_transfer_coding_decode_error::invalid_content);
}

transfer_coding_decoder::inflate_step_result transfer_coding_decoder::inflate_step(std::string_view input, std::span<char> output) noexcept {
    const auto input_bytes = std::min<std::size_t>(input.size(), (std::numeric_limits<uInt>::max)());
    auto& stream = stream_.native();
    stream.next_in = input_bytes == 0 ? Z_NULL : reinterpret_cast<Bytef*>(const_cast<char*>(input.data()));
    stream.avail_in = static_cast<uInt>(input_bytes);
    const auto output_bytes = std::min<std::size_t>(output.size(), (std::numeric_limits<uInt>::max)());
    stream.next_out = reinterpret_cast<Bytef*>(output.data());
    stream.avail_out = static_cast<uInt>(output_bytes);

    const auto status = inflate(&stream, Z_NO_FLUSH);
    const auto consumed = input_bytes - stream.avail_in;
    const auto produced = output_bytes - stream.avail_out;
    stream.next_in = Z_NULL;
    stream.avail_in = 0;
    stream.next_out = Z_NULL;
    stream.avail_out = 0;
    return inflate_step_result{consumed, produced, status};
}

http_transfer_coding_decode_result transfer_coding_decoder::need_input(std::size_t consumed) noexcept {
    return http_transfer_coding_decode_result(http_transfer_coding_decode_need_input(consumed));
}

http_transfer_coding_decode_result transfer_coding_decoder::output(std::size_t consumed, std::string_view bytes_value) noexcept {
    return http_transfer_coding_decode_result(http_transfer_coding_decode_output_view(consumed, bytes_value));
}

http_transfer_coding_decode_result transfer_coding_decoder::complete(std::size_t consumed) noexcept {
    return http_transfer_coding_decode_result(http_transfer_coding_decode_complete(consumed));
}

http_transfer_coding_decode_result transfer_coding_decoder::fail(std::size_t consumed, http_transfer_coding_decode_error error) noexcept {
    return http_transfer_coding_decode_result(http_transfer_coding_decode_failure(consumed, error));
}

http_transfer_coding_decode_result transfer_coding_decoder::fail_decoder(std::size_t consumed) noexcept {
    return http_transfer_coding_decode_result(http_transfer_coding_decoder_failure(consumed));
}

}  // namespace detail

namespace {

constexpr std::size_t transfer_coding_scratch_bytes = 4096;

struct transfer_coding_scratch_buffer final {
    // Stage output bounds valid bytes to [begin_, end_), so default construction
    // leaves the 4 KiB storage uninitialized instead of zero-filling it.
    std::array<char, transfer_coding_scratch_bytes> bytes_;
};

struct transfer_coding_stage final {
    transfer_coding_stage(http_transfer_coding coding, protocol_byte_limit decoded_limit) :
        decoder_(coding, decoded_limit) {}

    detail::transfer_coding_decoder decoder_;
    std::size_t begin_{0};
    std::size_t end_{0};
    bool complete_{false};
};

}  // namespace

struct http_transfer_coding_stack_decoder::impl_type final {
    impl_type(std::span<const http_transfer_coding> codings, protocol_byte_limit decoded_limit) :
        stage_count_(codings.size()) {
        std::size_t index = 0;
        for (auto coding = codings.rbegin(); coding != codings.rend(); ++coding) {
            stages_[index++].emplace(*coding, decoded_limit);
        }
    }
    std::array<std::optional<transfer_coding_stage>, max_transfer_codings> stages_{};
    std::size_t stage_count_{0};
    // Only inter-stage edges own scratch. The final stage writes directly to
    // the caller's buffer, so a one-coding sequence uses no internal scratch.
    std::array<transfer_coding_scratch_buffer, max_transfer_codings - 1> buffers_;
    std::optional<http_transfer_coding_decode_error> failure_;
    bool decoder_failed_{false};
};

http_transfer_coding_stack_decoder::http_transfer_coding_stack_decoder(std::span<const http_transfer_coding> codings, protocol_byte_limit decoded_limit) {
    if (codings.empty() || codings.size() > max_transfer_codings) {
        throw std::invalid_argument("invalid transfer-coding stack length");
    }
    impl_ = new impl_type(codings, decoded_limit);
}

http_transfer_coding_stack_decoder::~http_transfer_coding_stack_decoder() {
    delete impl_;
}

http_transfer_coding_decode_result http_transfer_coding_stack_decoder::decode(std::string_view input, std::span<char> output_buffer) noexcept {
    const auto failed = [this](std::size_t consumed) {
        if (impl_->decoder_failed_) {
            return http_transfer_coding_decode_result(http_transfer_coding_decoder_failure(consumed));
        }
        return http_transfer_coding_decode_result(http_transfer_coding_decode_failure(consumed, *impl_->failure_));
    };
    if (impl_->decoder_failed_ || impl_->failure_.has_value()) {
        return failed(0);
    }
    if (output_buffer.empty()) {
        impl_->decoder_failed_ = true;
        return http_transfer_coding_decode_result(http_transfer_coding_decoder_failure(0));
    }

    std::size_t wire_consumed = 0;
    for (;;) {
        bool progressed = false;
        for (std::size_t index = 0; index < impl_->stage_count_; ++index) {
            auto& stage = *impl_->stages_[index];
            if (index + 1 < impl_->stage_count_ && stage.begin_ != stage.end_) {
                continue;
            }
            std::string_view layer_input;
            if (index == 0) {
                layer_input = input.substr(wire_consumed);
            } else {
                auto& previous = *impl_->stages_[index - 1];
                layer_input = std::string_view(impl_->buffers_[index - 1].bytes_.data() + previous.begin_, previous.end_ - previous.begin_);
            }
            if (stage.complete_) {
                if (!layer_input.empty()) {
                    impl_->failure_ = http_transfer_coding_decode_error::invalid_content;
                    return failed(wire_consumed);
                }
                continue;
            }
            const auto output = index + 1 == impl_->stage_count_ ? output_buffer : std::span<char>(impl_->buffers_[index].bytes_);
            const auto decoded = stage.decoder_.decode(layer_input, output);
            const auto consumed = decoded.consumed_bytes();
            if (index == 0) {
                wire_consumed += consumed;
            } else {
                auto& previous = *impl_->stages_[index - 1];
                previous.begin_ += consumed;
                if (previous.begin_ == previous.end_) {
                    previous.begin_ = 0;
                    previous.end_ = 0;
                }
            }
            progressed = progressed || consumed != 0;
            if (const auto* bytes = decoded.output()) {
                progressed = true;
                if (index + 1 == impl_->stage_count_) {
                    return http_transfer_coding_decode_result(http_transfer_coding_decode_output_view(wire_consumed, bytes->bytes()));
                }
                stage.begin_ = 0;
                stage.end_ = bytes->bytes().size();
                continue;
            }
            if (const auto* protocol_failure = decoded.failure()) {
                impl_->failure_ = protocol_failure->error();
                return failed(wire_consumed);
            }
            if (decoded.decoder_failure() != nullptr) {
                impl_->decoder_failed_ = true;
                return failed(wire_consumed);
            }
            if (decoded.complete() != nullptr) {
                stage.complete_ = true;
                progressed = true;
            }
        }

        bool all_complete = true;
        bool pending = false;
        for (std::size_t index = 0; index < impl_->stage_count_; ++index) {
            const auto& stage = *impl_->stages_[index];
            all_complete = all_complete && stage.complete_;
            pending = pending || (index + 1 < impl_->stage_count_ && stage.begin_ != stage.end_);
        }
        if (all_complete && !pending) {
            return http_transfer_coding_decode_result(http_transfer_coding_decode_complete(wire_consumed));
        }
        if (!progressed) {
            return http_transfer_coding_decode_result(http_transfer_coding_decode_need_input(wire_consumed));
        }
    }
}

http_transfer_coding_decode_result http_transfer_coding_stack_decoder::finish_input() noexcept {
    if (impl_->decoder_failed_) {
        return http_transfer_coding_decode_result(http_transfer_coding_decoder_failure(0));
    }
    if (impl_->failure_.has_value()) {
        return http_transfer_coding_decode_result(http_transfer_coding_decode_failure(0, *impl_->failure_));
    }
    const bool wire_received_input = impl_->stages_[0]->decoder_.received_input();
    for (std::size_t index = 0; index < impl_->stage_count_; ++index) {
        auto& stage = *impl_->stages_[index];
        if (index + 1 < impl_->stage_count_ && stage.begin_ != stage.end_) {
            return http_transfer_coding_decode_result(http_transfer_coding_decode_need_input(0));
        }
        if (stage.complete_) {
            continue;
        }
        if (index > 0 && wire_received_input && !stage.decoder_.received_input()) {
            impl_->failure_ = http_transfer_coding_decode_error::invalid_content;
            return http_transfer_coding_decode_result(http_transfer_coding_decode_failure(0, *impl_->failure_));
        }
        const auto finished = stage.decoder_.finish_input();
        if (const auto* protocol_failure = finished.failure()) {
            impl_->failure_ = protocol_failure->error();
            return http_transfer_coding_decode_result(http_transfer_coding_decode_failure(0, *impl_->failure_));
        }
        if (finished.decoder_failure() != nullptr) {
            impl_->decoder_failed_ = true;
            return http_transfer_coding_decode_result(http_transfer_coding_decoder_failure(0));
        }
        stage.complete_ = true;
    }
    return http_transfer_coding_decode_result(http_transfer_coding_decode_complete(0));
}

}  // namespace ruvia
