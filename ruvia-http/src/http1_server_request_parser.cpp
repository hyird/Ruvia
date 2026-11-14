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
#include "http_response_server.h"
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

namespace ruvia {

std::string_view http1_client_response_body_error_message(http1_client_response_body_error error) noexcept {
    switch (error) {
        case http1_client_response_body_error::invalid_framing:
            return "invalid HTTP/1 response body framing";
        case http1_client_response_body_error::invalid_transfer_coding:
            return "invalid HTTP/1 response transfer coding";
        case http1_client_response_body_error::incomplete_body:
            return "incomplete HTTP/1 response body";
    }
    return "invalid HTTP/1 response body";
}

http1_client_response_body_decoder::http1_client_response_body_decoder(http1_client_response_plan plan) {
    if (plan.informational() || plan.connect_tunnel() || plan.protocol_upgrade()) {
        throw std::invalid_argument("HTTP/1 response plan has no ordinary message body");
    }
    if (const auto* without = plan.without_content()) {
        framing_ = framing_type::no_body;
        persistence_ = without->persistence();
        return;
    }
    // A discarding (205) plan carries the same framings as an ordinary one,
    // but its content is never emitted, so it is never transfer-decoded.
    discard_content_ = plan.discards_content();
    const auto configure_coding = [this](const http_transfer_codings& codings) {
        if (!codings.empty() && !discard_content_) {
            transfer_.emplace(std::span<const http_transfer_coding>(codings.data(), codings.size()), protocol_byte_limit::unlimited());
        }
    };
    if (const auto* known = plan.known_length()) {
        framing_ = framing_type::fixed;
        remaining_ = known->content_length();
        persistence_ = known->persistence();
    } else if (const auto* chunked = plan.chunked()) {
        framing_ = framing_type::chunked;
        persistence_ = chunked->persistence();
        configure_coding(chunked->transfer_codings());
        chunked_.emplace(http1_chunked_body_decoder_config{.obs_fold_ = http1_trailer_obs_fold::accept, .trailer_message_ = http1_trailer_message::response});
    } else if (const auto* close = plan.close_delimited()) {
        framing_ = framing_type::close_delimited;
        configure_coding(close->transfer_codings());
    } else {
        throw std::invalid_argument("unsupported HTTP/1 response plan");
    }
}

http1_client_response_body_decoder::result_type http1_client_response_body_decoder::decode(std::string_view available, std::span<char> scratch) {
    if (scratch.empty()) {
        throw std::invalid_argument("HTTP/1 response body decoder scratch must be non-empty");
    }
    if (terminal_) {
        return replay_terminal();
    }
    return step(available, scratch, eof_);
}

http1_client_response_body_decoder::result_type http1_client_response_body_decoder::finish_input(std::string_view available, std::span<char> scratch) {
    if (scratch.empty()) {
        throw std::invalid_argument("HTTP/1 response body decoder scratch must be non-empty");
    }
    if (terminal_) {
        return replay_terminal();
    }
    eof_ = true;
    return step(available, scratch, true);
}

http1_client_response_body_decoder::result_type http1_client_response_body_decoder::step(std::string_view available, std::span<char> scratch, bool eof) {
    if (!discard_content_) {
        return step_content(available, scratch, eof);
    }
    // RFC 9110 section 15.3.6 forbids only generating 205 content: consume
    // what the framing delimits without emitting it, then report the next
    // non-output result over every byte consumed by this call.
    std::size_t discarded = 0;
    for (;;) {
        const auto result_value = step_content(available.substr(discarded), scratch, eof);
        if (result_value.output() == nullptr) {
            return with_consumed_prefix(result_value, discarded);
        }
        discarded += result_value.consumed_bytes();
    }
}

http1_client_response_body_decoder::result_type http1_client_response_body_decoder::step_content(std::string_view available, std::span<char> scratch, bool eof) {
    if (trailers_reported_) {
        return finish_transfer(0, scratch);
    }
    // A full output step can leave inflate output buffered after consuming
    // all wire input. Drain it before asking framing or transport for more.
    if (transfer_ && transfer_phase_ == transfer_phase_type::drain_output) {
        const auto pending = drive_transfer({}, 0, scratch);
        if (pending.need_input() == nullptr) {
            return pending;
        }
    }
    switch (framing_) {
        case framing_type::fixed:
            return step_fixed(available, scratch, eof);
        case framing_type::close_delimited:
            return step_close_delimited(available, scratch, eof);
        case framing_type::chunked:
            return step_chunked(available, scratch, eof);
        case framing_type::no_body:
            break;
    }
    return complete(0);
}

http1_client_response_body_decoder::result_type http1_client_response_body_decoder::step_fixed(std::string_view available, std::span<char> scratch, bool eof) {
    if (remaining_ == 0) {
        return complete(0);
    }
    if (available.empty()) {
        return need_input_or_incomplete(0, eof);
    }
    const auto output_length = std::min({remaining_, available.size(), scratch.size()});
    remaining_ -= output_length;
    return emit(output_length, available.substr(0, output_length));
}

http1_client_response_body_decoder::result_type http1_client_response_body_decoder::step_close_delimited(std::string_view available, std::span<char> scratch, bool eof) {
    if (!transfer_) {
        if (available.empty()) {
            return eof ? complete(0) : need_input(0);
        }
        const auto output_length = std::min(available.size(), scratch.size());
        return emit(output_length, available.substr(0, output_length));
    }
    std::size_t consumed = 0;
    for (;;) {
        const auto rest = available.substr(consumed);
        if (transfer_phase_ == transfer_phase_type::ended) {
            // The connection close is the only valid framing end after the
            // transfer coding completed.
            if (!rest.empty()) {
                return fail(http1_client_response_body_error::invalid_transfer_coding, consumed);
            }
            return eof ? complete(consumed) : need_input(consumed);
        }
        if (rest.empty()) {
            return eof ? finish_transfer(consumed, scratch) : need_input(consumed);
        }
        const auto driven = drive_transfer(rest, consumed, scratch);
        const auto input_consumed = driven.consumed_bytes() - consumed;
        consumed += input_consumed;
        if (driven.need_input() == nullptr) {
            return driven;
        }
        if (input_consumed == 0 && transfer_phase_ != transfer_phase_type::ended) {
            return need_input_or_incomplete(consumed, eof);
        }
    }
}

http1_client_response_body_decoder::result_type http1_client_response_body_decoder::step_chunked(std::string_view available, std::span<char> scratch, bool eof) {
    std::size_t consumed = 0;
    for (;;) {
        if (auto pending = drain_pending_chunk(available, consumed, scratch, eof)) {
            return *pending;
        }
        const auto rest = available.substr(consumed);
        const auto decoded = chunked_->decode(rest, transfer_ ? (std::numeric_limits<std::size_t>::max)() : scratch.size());
        const auto chunk_consumed = consumed + decoded.consumed_bytes();
        if (decoded.failure() != nullptr) {
            return fail(http1_client_response_body_error::invalid_framing, chunk_consumed);
        }
        if (decoded.need_more() != nullptr) {
            return need_input_or_incomplete(chunk_consumed, eof);
        }
        if (const auto* complete = decoded.complete()) {
            trailers_reported_ = true;
            return validated_trailers(chunk_consumed, complete->trailers());
        }
        const auto body = decoded.body_chunk()->bytes();
        if (!transfer_) {
            return emit(chunk_consumed, body);
        }
        // The chunk decoder already consumed the whole chunk, but the transfer
        // decoder may take only part of its data. Track the rest as wire bytes
        // so the caller keeps the unconsumed suffix without a borrowed view.
        consumed += static_cast<std::size_t>(body.data() - rest.data());
        pending_body_bytes_ = body.size();
        pending_delimiter_bytes_ = chunk_consumed - consumed - body.size();
    }
}

// Feeds pending chunk data to the transfer decoder and then skips the chunk
// delimiter. Returns nothing once both are consumed and framing can continue.
std::optional<http1_client_response_body_decoder::result_type> http1_client_response_body_decoder::drain_pending_chunk(std::string_view available, std::size_t& consumed, std::span<char> scratch, bool eof) {
    if (pending_body_bytes_ != 0) {
        const auto input = available.substr(consumed, std::min(pending_body_bytes_, available.size() - consumed));
        const auto driven = drive_transfer(input, consumed, scratch);
        const auto input_consumed = driven.consumed_bytes() - consumed;
        pending_body_bytes_ -= input_consumed;
        consumed += input_consumed;
        if (pending_body_bytes_ == 0) {
            consume_pending_delimiter(available, consumed);
        }
        if (driven.need_input() == nullptr) {
            return with_consumed(driven, consumed);
        }
        if (pending_body_bytes_ != 0 || consumed == available.size()) {
            return need_input_or_incomplete(consumed, eof);
        }
    } else {
        consume_pending_delimiter(available, consumed);
    }
    if (pending_delimiter_bytes_ != 0) {
        return need_input_or_incomplete(consumed, eof);
    }
    return std::nullopt;
}

// The chunk decoder already validated these delimiter bytes.
void http1_client_response_body_decoder::consume_pending_delimiter(std::string_view available, std::size_t& consumed) noexcept {
    const auto bytes = std::min(pending_delimiter_bytes_, available.size() - consumed);
    consumed += bytes;
    pending_delimiter_bytes_ -= bytes;
}

http1_client_response_body_decoder::result_type http1_client_response_body_decoder::drive_transfer(std::string_view input, std::size_t wire_prefix, std::span<char> scratch) {
    const auto decoded = transfer_->decode(input, scratch);
    const auto consumed = wire_prefix + decoded.consumed_bytes();
    if (const auto* output = decoded.output()) {
        return emit_transfer_output(consumed, output->bytes());
    }
    if (decoded.need_input() != nullptr) {
        transfer_phase_ = transfer_phase_type::needs_framed_input;
        return need_input(consumed);
    }
    if (decoded.complete() != nullptr) {
        transfer_phase_ = transfer_phase_type::ended;
        return need_input(consumed);
    }
    if (decoded.failure() != nullptr) {
        return fail(http1_client_response_body_error::invalid_transfer_coding, consumed);
    }
    return decoder_failure(consumed);
}

http1_client_response_body_decoder::result_type http1_client_response_body_decoder::finish_transfer(std::size_t consumed, std::span<char> scratch) {
    if (!transfer_) {
        return complete(consumed);
    }
    // Buffered output drains before framing EOF is committed to the decoder.
    auto decoded = transfer_->decode({}, scratch);
    if (decoded.need_input() != nullptr) {
        decoded = transfer_->finish_input();
    }
    if (const auto* output = decoded.output()) {
        return emit_transfer_output(consumed, output->bytes());
    }
    if (decoded.complete() != nullptr) {
        transfer_phase_ = transfer_phase_type::ended;
        return complete(consumed);
    }
    if (decoded.decoder_failure() != nullptr) {
        return decoder_failure(consumed);
    }
    return fail(http1_client_response_body_error::incomplete_body, consumed);
}

http1_client_response_body_decoder::result_type http1_client_response_body_decoder::replay_terminal() const noexcept {
    return std::visit([](const auto& payload_value) -> result_type {
        using t_type = std::decay_t<decltype(payload_value)>;
        if constexpr (std::is_same_v<t_type, complete_type>) {
            return result_type(complete_type(0, payload_value.persistence()));
        } else if constexpr (std::is_same_v<t_type, protocol_failure_type>) {
            return result_type(protocol_failure_type(0, payload_value.error()));
        } else {
            static_assert(std::is_same_v<t_type, decoder_failure_type>);
            return result_type(decoder_failure_type(0));
        }
    }, *terminal_);
}

http1_client_response_body_decoder::result_type http1_client_response_body_decoder::need_input(std::size_t consumed) const noexcept {
    return result_type(need_input_type(consumed));
}

http1_client_response_body_decoder::result_type http1_client_response_body_decoder::need_input_or_incomplete(std::size_t consumed, bool eof) {
    if (eof) {
        return fail(http1_client_response_body_error::incomplete_body, consumed);
    }
    return need_input(consumed);
}

// The single emission point.
http1_client_response_body_decoder::result_type http1_client_response_body_decoder::emit(std::size_t consumed, std::string_view bytes_value) {
    return result_type(output_view_type(consumed, bytes_value));
}

// Transfer output borrows scratch and must be drained before more framed input.
http1_client_response_body_decoder::result_type http1_client_response_body_decoder::emit_transfer_output(std::size_t consumed, std::string_view bytes_value) {
    transfer_phase_ = transfer_phase_type::drain_output;
    return emit(consumed, bytes_value);
}

// Re-reports a transfer result with the wire bytes consumed after its input,
// such as the chunk delimiter that follows the final chunk data.
http1_client_response_body_decoder::result_type http1_client_response_body_decoder::with_consumed(const result_type& result, std::size_t consumed) noexcept {
    return std::visit([consumed](const auto& payload_value) -> result_type {
        using t_type = std::decay_t<decltype(payload_value)>;
        if constexpr (std::is_same_v<t_type, output_view_type>) {
            return result_type(output_view_type(consumed, payload_value.bytes()));
        } else if constexpr (std::is_same_v<t_type, protocol_failure_type>) {
            return result_type(protocol_failure_type(consumed, payload_value.error()));
        } else if constexpr (std::is_same_v<t_type, decoder_failure_type>) {
            return result_type(decoder_failure_type(consumed));
        } else {
            return result_type(payload_value);
        }
    }, result.value_);
}

http1_client_response_body_decoder::result_type http1_client_response_body_decoder::with_consumed_prefix(const result_type& result, std::size_t prefix) noexcept {
    return std::visit([prefix](const auto& payload_value) -> result_type {
        using t_type = std::decay_t<decltype(payload_value)>;
        const auto consumed = prefix + payload_value.consumed_bytes();
        if constexpr (std::is_same_v<t_type, need_input_type>) {
            return result_type(need_input_type(consumed));
        } else if constexpr (std::is_same_v<t_type, output_view_type>) {
            return result_type(output_view_type(consumed, payload_value.bytes()));
        } else if constexpr (std::is_same_v<t_type, validated_trailers_view_type>) {
            return result_type(validated_trailers_view_type(consumed, payload_value.fields()));
        } else if constexpr (std::is_same_v<t_type, complete_type>) {
            return result_type(complete_type(consumed, payload_value.persistence()));
        } else if constexpr (std::is_same_v<t_type, protocol_failure_type>) {
            return result_type(protocol_failure_type(consumed, payload_value.error()));
        } else {
            static_assert(std::is_same_v<t_type, decoder_failure_type>);
            return result_type(decoder_failure_type(consumed));
        }
    }, result.value_);
}

http1_client_response_body_decoder::result_type http1_client_response_body_decoder::validated_trailers(std::size_t consumed, std::span<const http_header> fields) {
    return result_type(validated_trailers_view_type(consumed, fields));
}

http1_client_response_body_decoder::result_type http1_client_response_body_decoder::complete(std::size_t consumed) {
    const auto completed = complete_type(consumed, persistence_);
    terminal_ = completed;
    return result_type(completed);
}

http1_client_response_body_decoder::result_type http1_client_response_body_decoder::fail(http1_client_response_body_error error, std::size_t consumed) {
    const auto failure = protocol_failure_type(consumed, error);
    terminal_ = failure;
    return result_type(failure);
}

http1_client_response_body_decoder::result_type http1_client_response_body_decoder::decoder_failure(std::size_t consumed) {
    const auto failure = decoder_failure_type(consumed);
    terminal_ = failure;
    return result_type(failure);
}

std::size_t http1_client_response_body_decoder::result_type::consumed_bytes() const noexcept {
    return std::visit([](const auto& payload_value) {
        return payload_value.consumed_bytes();
    }, value_);
}

const http1_client_response_body_decoder::need_input_type* http1_client_response_body_decoder::result_type::need_input() const& noexcept {
    return std::get_if<need_input_type>(&value_);
}

const http1_client_response_body_decoder::output_view_type* http1_client_response_body_decoder::result_type::output() const& noexcept {
    return std::get_if<output_view_type>(&value_);
}

const http1_client_response_body_decoder::validated_trailers_view_type* http1_client_response_body_decoder::result_type::trailers() const& noexcept {
    return std::get_if<validated_trailers_view_type>(&value_);
}

const http1_client_response_body_decoder::complete_type* http1_client_response_body_decoder::result_type::complete() const& noexcept {
    return std::get_if<complete_type>(&value_);
}

const http1_client_response_body_decoder::protocol_failure_type* http1_client_response_body_decoder::result_type::protocol_failure() const& noexcept {
    return std::get_if<protocol_failure_type>(&value_);
}

const http1_client_response_body_decoder::decoder_failure_type* http1_client_response_body_decoder::result_type::decoder_failure() const& noexcept {
    return std::get_if<decoder_failure_type>(&value_);
}

}  // namespace ruvia

// The Close frame payload (RFC 6455 section 5.5.1): encoding the one an
// endpoint sends (a validated status code followed by an optional UTF-8
// reason, capped at the 125-byte control-frame limit) and checking the one a
// peer sent.

namespace ruvia::detail {

enum class websocket_close_payload_encode_error : std::uint8_t {
    invalid_reason,
    reason_too_large,
};

class websocket_close_payload_encode_result;

class websocket_encoded_close_payload final {
public:
    [[nodiscard]] constexpr std::string_view bytes() const& noexcept {
        return std::string_view(bytes_.data(), size_);
    }
    [[nodiscard]] constexpr std::string_view bytes() const&& = delete;

private:
    friend class websocket_close_payload_encode_result;
    friend websocket_close_payload_encode_result encode_websocket_close_payload(websocket_close_code, std::string_view) noexcept;

    websocket_encoded_close_payload(websocket_close_code code, std::string_view reason) noexcept;

    std::array<char, 125> bytes_;
    std::uint8_t size_{0};
};

class websocket_close_payload_encode_failure final {
public:
    [[nodiscard]] constexpr websocket_close_payload_encode_error error() const noexcept {
        return error_;
    }

private:
    friend class websocket_close_payload_encode_result;
    friend websocket_close_payload_encode_result encode_websocket_close_payload(websocket_close_code, std::string_view) noexcept;

    explicit constexpr websocket_close_payload_encode_failure(websocket_close_payload_encode_error error) noexcept :
        error_(error) {}

    websocket_close_payload_encode_error error_;
};

class websocket_close_payload_encode_result final {
public:
    [[nodiscard]] constexpr const websocket_encoded_close_payload* encoded() const& noexcept {
        return (value_.index() == 0) ? &std::get<0>(value_) : nullptr;
    }
    [[nodiscard]] constexpr const websocket_encoded_close_payload* encoded() const&& = delete;

    [[nodiscard]] constexpr const websocket_close_payload_encode_failure* failure() const& noexcept {
        return (value_.index() == 0) ? nullptr : &std::get<1>(value_);
    }
    [[nodiscard]] constexpr const websocket_close_payload_encode_failure* failure() const&& = delete;

private:
    friend websocket_close_payload_encode_result encode_websocket_close_payload(websocket_close_code, std::string_view) noexcept;

    explicit websocket_close_payload_encode_result(websocket_encoded_close_payload encoded) noexcept :
        value_(encoded) {}

    explicit constexpr websocket_close_payload_encode_result(websocket_close_payload_encode_failure failure) noexcept :
        value_(failure) {}

    using value_type = std::variant<websocket_encoded_close_payload, websocket_close_payload_encode_failure>;
    value_type value_;
};

[[nodiscard]] websocket_close_payload_encode_result encode_websocket_close_payload(websocket_close_code code, std::string_view reason) noexcept;

// The protocol failure a received Close payload commits, or nullopt when it is legal.
[[nodiscard]] std::optional<websocket_protocol_failure> websocket_close_payload_failure(std::string_view payload_value) noexcept;
}  // namespace ruvia::detail

namespace ruvia::detail {

websocket_encoded_close_payload::websocket_encoded_close_payload(websocket_close_code code, std::string_view reason) noexcept :
    size_(static_cast<std::uint8_t>(reason.size() + 2)) {
    const auto code_value = code.value();
    bytes_[0] = static_cast<char>((code_value >> 8) & 0xFF);
    bytes_[1] = static_cast<char>(code_value & 0xFF);
    if (!reason.empty()) {
        std::memcpy(bytes_.data() + 2, reason.data(), reason.size());
    }
}

websocket_close_payload_encode_result encode_websocket_close_payload(websocket_close_code code, std::string_view reason) noexcept {
    if (reason.size() > 123) {
        return websocket_close_payload_encode_result(websocket_close_payload_encode_failure(websocket_close_payload_encode_error::reason_too_large));
    }
    if (!::ruvia::is_valid_utf8(reason)) {
        return websocket_close_payload_encode_result(websocket_close_payload_encode_failure(websocket_close_payload_encode_error::invalid_reason));
    }
    return websocket_close_payload_encode_result(websocket_encoded_close_payload(code, reason));
}

std::optional<websocket_protocol_failure> websocket_close_payload_failure(std::string_view payload_value) noexcept {
    // A malformed frame is a protocol error (close 1002); an otherwise-valid
    // frame whose reason is not valid UTF-8 is invalid payload data (close
    // 1007, RFC 6455 section 8.1).
    if (payload_value.size() == 1) {
        return websocket_protocol_failure::protocol_error;
    }
    if (payload_value.size() < 2) {
        return std::nullopt;
    }
    if (!websocket_close_code::is_valid_value(read_websocket_uint16(payload_value.data()))) {
        return websocket_protocol_failure::protocol_error;
    }
    if (!::ruvia::is_valid_utf8(payload_value.substr(2))) {
        return websocket_protocol_failure::invalid_payload_data;
    }
    return std::nullopt;
}

class websocket_frame_read_result;

// One borrowed frame published by the wire reader, which owns masking, length,
// opcode/compression combinations, and Close payload validation. Payload
// storage must outlive the view.
class websocket_frame_view final {
public:
    [[nodiscard]] constexpr websocket_frame_kind kind() const noexcept {
        return kind_;
    }

    [[nodiscard]] constexpr std::string_view payload() const noexcept {
        return payload_;
    }

    [[nodiscard]] constexpr bool final() const noexcept {
        return final_;
    }

    [[nodiscard]] constexpr bool compressed() const noexcept {
        return compressed_;
    }

private:
    friend class websocket_frame_read_result;

    constexpr websocket_frame_view(const websocket_frame_start& start, std::string_view payload_value) noexcept :
        kind_(start.kind()),
        payload_(payload_value),
        final_(start.final()),
        compressed_(start.compressed()) {}

    websocket_frame_kind kind_;
    std::string_view payload_;
    bool final_;
    bool compressed_;
};

// Drop the bytes the previously returned frame occupied. Consuming never moves
// bytes; the buffer moves the unread tail only when a later read needs room.
// A drained buffer keeps at most websocket_retained_buffer_bytes, so a burst
// of large messages does not stay pinned to the connection.
inline void consume_websocket_read_frame(http_io_buffer& buffer, std::size_t& pending_consume) noexcept {
    if (pending_consume == 0) {
        return;
    }
    buffer.consume(std::exchange(pending_consume, 0));
    if (buffer.empty()) {
        buffer.clear_retaining_small(websocket_retained_buffer_bytes);
    }
}

class websocket_frame_need_input final {
public:
    // Bytes the frame still lacks once its header is complete; 0 while the
    // header itself is incomplete. The header length is already within the
    // message limit.
    [[nodiscard]] constexpr std::size_t missing_bytes() const noexcept {
        return missing_bytes_;
    }

private:
    friend class websocket_frame_read_result;
    explicit constexpr websocket_frame_need_input(std::size_t missing_bytes) noexcept :
        missing_bytes_(missing_bytes) {}

    std::size_t missing_bytes_;
};

class websocket_frame_read_failure final {
public:
    [[nodiscard]] constexpr websocket_protocol_failure error() const noexcept {
        return error_;
    }

private:
    friend class websocket_frame_read_result;

    explicit constexpr websocket_frame_read_failure(websocket_protocol_failure error) noexcept :
        error_(error) {}

    websocket_protocol_failure error_;
};

// Parsing either needs more input, exposes one complete borrowed frame, or reports
// one typed protocol failure. No default frame, EOF boolean, or wire-format
// exception can coexist with another outcome; only need_input() carries a
// byte count.
class websocket_frame_read_result final {
public:
    [[nodiscard]] constexpr const websocket_frame_need_input* need_input() const& noexcept {
        return std::get_if<websocket_frame_need_input>(&value_);
    }
    [[nodiscard]] constexpr const websocket_frame_need_input* need_input() const&& = delete;

    [[nodiscard]] constexpr const websocket_frame_view* frame() const& noexcept {
        return std::get_if<websocket_frame_view>(&value_);
    }
    [[nodiscard]] constexpr const websocket_frame_view* frame() const&& = delete;

    [[nodiscard]] constexpr const websocket_frame_read_failure* failure() const& noexcept {
        return std::get_if<websocket_frame_read_failure>(&value_);
    }
    [[nodiscard]] constexpr const websocket_frame_read_failure* failure() const&& = delete;

private:
    friend websocket_frame_read_result websocket_try_read_frame(http_io_buffer&, std::size_t&, protocol_byte_limit, bool, bool);

    using value_type = std::variant<websocket_frame_need_input, websocket_frame_view, websocket_frame_read_failure>;

    template <typename alternative_type>
    explicit constexpr websocket_frame_read_result(alternative_type alternative) noexcept :
        value_(alternative) {}

    [[nodiscard]] static constexpr websocket_frame_read_result make_need_input(std::size_t missing_bytes = 0) noexcept {
        return websocket_frame_read_result(websocket_frame_need_input(missing_bytes));
    }

    [[nodiscard]] static constexpr websocket_frame_read_result make_frame(const websocket_frame_start& start, std::string_view payload_value) noexcept {
        return websocket_frame_read_result(websocket_frame_view(start, payload_value));
    }

    [[nodiscard]] static constexpr websocket_frame_read_result make_failure(websocket_protocol_failure error) noexcept {
        return websocket_frame_read_result(websocket_frame_read_failure(error));
    }

    value_type value_;
};

// Single owner of websocket frame decode (RFC 6455 §5.2): FIN/opcode/length
// parsing, role-correct masking validation, control-frame and length-limit
// validation, and in-place unmasking when the peer is a client.
// It never performs I/O and never throws for peer bytes; callers append transport
// input after need_input(), while failure() carries the Close reason. A returned
// frame stays in the buffer, counted by `pending_consume`, until the next call.
[[nodiscard]] inline websocket_frame_read_result websocket_try_read_frame(http_io_buffer& buffer, std::size_t& pending_consume, protocol_byte_limit message_limit, bool permessage_deflate, bool expect_masked) {
    consume_websocket_read_frame(buffer, pending_consume);
    const auto available = buffer.size();
    if (available < 2) {
        return websocket_frame_read_result::make_need_input();
    }
    const auto first = static_cast<unsigned char>(buffer.data()[0]);
    const auto second = static_cast<unsigned char>(buffer.data()[1]);
    std::uint64_t length = second & 0x7FU;
    std::size_t header_size = 2;

    const auto frame_start = decode_websocket_frame_start(first, second, permessage_deflate, expect_masked);
    if (!frame_start.has_value()) {
        return websocket_frame_read_result::make_failure(websocket_protocol_failure::protocol_error);
    }
    // RFC 6455 section 5.2 obliges the sender to use the minimal length form;
    // the frame stays unambiguous either way, so a receiver does not reject a
    // longer form (sender MUST, not recipient rule).
    if (length == 126) {
        if (available < header_size + 2) {
            return websocket_frame_read_result::make_need_input();
        }
        length = read_websocket_uint16(buffer.data() + header_size);
        header_size += 2;
    } else if (length == 127) {
        if (available < header_size + 8) {
            return websocket_frame_read_result::make_need_input();
        }
        const auto decoded_length = read_websocket_uint64(std::span<const char, 8>(buffer.data() + header_size, 8));
        if ((decoded_length.index() != 0)) {
            return websocket_frame_read_result::make_failure(std::get<1>(decoded_length));
        }
        length = std::get<0>(decoded_length);
        header_size += 8;
    }

    if (is_invalid_websocket_control_frame(*frame_start, length)) {
        return websocket_frame_read_result::make_failure(websocket_protocol_failure::protocol_error);
    }
    if (websocket_frame_exceeds_message_limit(frame_start->kind(), length, message_limit)) {
        return websocket_frame_read_result::make_failure(websocket_protocol_failure::message_too_large);
    }
    const auto mask_bytes = expect_masked ? std::size_t{4} : std::size_t{0};
    // header_size is at most 10, so only the payload length can overflow.
    if (length > static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)() - header_size - mask_bytes)) {
        return websocket_frame_read_result::make_failure(websocket_protocol_failure::protocol_error);
    }
    const auto total_frame_bytes = header_size + mask_bytes + static_cast<std::size_t>(length);
    if (available < total_frame_bytes) {
        return websocket_frame_read_result::make_need_input(total_frame_bytes - available);
    }

    const auto mask_offset = header_size;
    const auto payload_offset = mask_offset + mask_bytes;
    const auto payload_size = static_cast<std::size_t>(length);
    auto* payload_value = buffer.data() + payload_offset;
    if (expect_masked) {
        decode_masked_websocket_payload(payload_value, payload_size, buffer.data() + mask_offset);
    }
    const auto payload_view = std::string_view(payload_value, payload_size);
    if (frame_start->kind() == websocket_frame_kind::close) {
        if (const auto failure = websocket_close_payload_failure(payload_view)) {
            return websocket_frame_read_result::make_failure(*failure);
        }
    }
    pending_consume = total_frame_bytes;
    return websocket_frame_read_result::make_frame(*frame_start, payload_view);
}

class websocket_inbound_result;

class websocket_inbound_continue final {
private:
    friend class websocket_inbound_result;
    constexpr websocket_inbound_continue() noexcept = default;
};

class websocket_inbound_control_frame final {
public:
    [[nodiscard]] constexpr websocket_opcode opcode() const noexcept {
        return opcode_;
    }

    [[nodiscard]] constexpr std::string_view payload() const noexcept {
        return payload_;
    }

private:
    friend class websocket_inbound_result;

    constexpr websocket_inbound_control_frame(websocket_opcode opcode, std::string_view payload_value) noexcept :
        opcode_(opcode),
        payload_(payload_value) {}

    websocket_opcode opcode_;
    std::string_view payload_;
};

enum class websocket_inbound_content_encoding : std::uint8_t {
    identity,
    per_message_deflate,
};

class websocket_inbound_message final {
public:
    [[nodiscard]] constexpr const websocket_message& message() const& noexcept {
        return message_;
    }
    [[nodiscard]] constexpr const websocket_message& message() const&& = delete;

    [[nodiscard]] constexpr websocket_inbound_content_encoding content_encoding() const noexcept {
        return content_encoding_;
    }

private:
    friend class websocket_inbound_result;

    constexpr websocket_inbound_message(websocket_message message, websocket_inbound_content_encoding content_encoding) noexcept :
        message_(message),
        content_encoding_(content_encoding) {}

    websocket_message message_;
    websocket_inbound_content_encoding content_encoding_;
};

class websocket_inbound_failure final {
public:
    [[nodiscard]] constexpr websocket_protocol_failure error() const noexcept {
        return error_;
    }

private:
    friend class websocket_inbound_result;

    explicit constexpr websocket_inbound_failure(websocket_protocol_failure error) noexcept :
        error_(error) {}

    websocket_protocol_failure error_;
};

// A consumed frame has exactly one semantic outcome. Control payload, application
// message, and failure code live only on their corresponding alternatives; there
// is no action enum coupled to an output parameter.
class websocket_inbound_result final {
public:
    [[nodiscard]] constexpr const websocket_inbound_continue* continue_reading() const& noexcept {
        return std::get_if<websocket_inbound_continue>(&value_);
    }
    [[nodiscard]] constexpr const websocket_inbound_continue* continue_reading() const&& = delete;

    [[nodiscard]] constexpr const websocket_inbound_control_frame* control_frame() const& noexcept {
        return std::get_if<websocket_inbound_control_frame>(&value_);
    }
    [[nodiscard]] constexpr const websocket_inbound_control_frame* control_frame() const&& = delete;

    [[nodiscard]] constexpr const websocket_inbound_message* message() const& noexcept {
        return std::get_if<websocket_inbound_message>(&value_);
    }
    [[nodiscard]] constexpr const websocket_inbound_message* message() const&& = delete;

    [[nodiscard]] constexpr const websocket_inbound_failure* failure() const& noexcept {
        return std::get_if<websocket_inbound_failure>(&value_);
    }
    [[nodiscard]] constexpr const websocket_inbound_failure* failure() const&& = delete;

private:
    friend class websocket_inbound_assembler;

    using value_type = std::variant<websocket_inbound_continue, websocket_inbound_control_frame, websocket_inbound_message, websocket_inbound_failure>;

    template <typename alternative_type>
    explicit constexpr websocket_inbound_result(alternative_type alternative) noexcept :
        value_(alternative) {}

    [[nodiscard]] static constexpr websocket_inbound_result make_continue() noexcept {
        return websocket_inbound_result(websocket_inbound_continue());
    }

    [[nodiscard]] static constexpr websocket_inbound_result make_control_frame(websocket_opcode opcode, std::string_view payload_value) noexcept {
        return websocket_inbound_result(websocket_inbound_control_frame(opcode, payload_value));
    }

    [[nodiscard]] static constexpr websocket_inbound_result make_message(websocket_message message, websocket_inbound_content_encoding content_encoding) noexcept {
        return websocket_inbound_result(websocket_inbound_message(message, content_encoding));
    }

    [[nodiscard]] static constexpr websocket_inbound_result make_failure(websocket_protocol_failure error) noexcept {
        return websocket_inbound_result(websocket_inbound_failure(error));
    }

    value_type value_;
};

// Single owner of the websocket inbound message-reassembly state machine (RFC
// 6455 §5.4): control-frame dispatch, fragmentation across continuation frames,
// per-message size limits and UTF-8 validation. Every wire failure is returned as
// a typed alternative carrying the exact RFC Close reason; none is thrown.
struct websocket_inbound_idle final {};

class websocket_inbound_fragmented final {
public:
    constexpr websocket_inbound_fragmented(websocket_opcode opcode, websocket_inbound_content_encoding encoding) noexcept :
        opcode_(opcode),
        encoding_(encoding) {}

    [[nodiscard]] constexpr websocket_opcode opcode() const noexcept {
        return opcode_;
    }
    [[nodiscard]] constexpr websocket_inbound_content_encoding encoding() const noexcept {
        return encoding_;
    }

private:
    websocket_opcode opcode_;
    websocket_inbound_content_encoding encoding_;
};

class websocket_inbound_assembler final {
public:
    websocket_inbound_assembler() = default;

    // A delivered view expires at the next poll, while a fragmented message
    // remains pinned until its final continuation or connection teardown. A
    // delivered message keeps only a bounded scratch, so streams of small
    // fragmented messages do not allocate per message and large ones are not
    // pinned to the connection.
    void release_completed() noexcept {
        if (std::holds_alternative<websocket_inbound_idle>(state_)) {
            http_clear_string_retaining_small(message_, websocket_retained_buffer_bytes);
        }
    }

    [[nodiscard]] websocket_inbound_result accept(const websocket_frame_view& frame, protocol_byte_limit message_limit) {
        if (is_websocket_control_frame_kind(frame.kind())) {
            const auto opcode = static_cast<websocket_opcode>(frame.kind());
            return websocket_inbound_result::make_control_frame(opcode, frame.payload());
        }
        if (frame.kind() == websocket_frame_kind::continuation) {
            const auto* fragmented = std::get_if<websocket_inbound_fragmented>(&state_);
            if (fragmented == nullptr) {
                return websocket_inbound_result::make_failure(websocket_protocol_failure::protocol_error);
            }
            if (message_limit.addition_exceeds(message_.size(), frame.payload().size())) {
                return websocket_inbound_result::make_failure(websocket_protocol_failure::message_too_large);
            }
            message_.append(frame.payload().data(), frame.payload().size());
            if (!frame.final()) {
                return websocket_inbound_result::make_continue();
            }
            const auto opcode = fragmented->opcode();
            const auto encoding = fragmented->encoding();
            state_.template emplace<websocket_inbound_idle>();
            const auto message = websocket_message::borrow(opcode, std::string_view(message_));
            if (encoding == websocket_inbound_content_encoding::per_message_deflate) {
                return websocket_inbound_result::make_message(message, websocket_inbound_content_encoding::per_message_deflate);
            }
            if (opcode == websocket_opcode::text && !::ruvia::is_valid_utf8(message_)) {
                return websocket_inbound_result::make_failure(websocket_protocol_failure::invalid_payload_data);
            }
            return websocket_inbound_result::make_message(message, websocket_inbound_content_encoding::identity);
        }
        if (frame.kind() == websocket_frame_kind::text || frame.kind() == websocket_frame_kind::binary) {
            if (std::get_if<websocket_inbound_fragmented>(&state_) != nullptr) {
                return websocket_inbound_result::make_failure(websocket_protocol_failure::protocol_error);
            }
            // websocket_try_read_frame already bounded this first frame by the message limit.
            const auto opcode = static_cast<websocket_opcode>(frame.kind());
            if (frame.final()) {
                const auto message = websocket_message::borrow(opcode, frame.payload());
                if (frame.compressed()) {
                    return websocket_inbound_result::make_message(message, websocket_inbound_content_encoding::per_message_deflate);
                }
                if (opcode == websocket_opcode::text && !::ruvia::is_valid_utf8(frame.payload())) {
                    return websocket_inbound_result::make_failure(websocket_protocol_failure::invalid_payload_data);
                }
                return websocket_inbound_result::make_message(message, websocket_inbound_content_encoding::identity);
            }
            message_.assign(frame.payload().data(), frame.payload().size());
            state_.template emplace<websocket_inbound_fragmented>(opcode, frame.compressed() ? websocket_inbound_content_encoding::per_message_deflate : websocket_inbound_content_encoding::identity);
        }
        return websocket_inbound_result::make_continue();
    }

private:
    std::string message_;
    std::variant<websocket_inbound_idle, websocket_inbound_fragmented> state_;
};

}  // namespace ruvia::detail
namespace ruvia {

namespace {

enum class websocket_payload_storage : std::uint8_t {
    copy,
    borrow,
};

[[nodiscard]] constexpr websocket_termination websocket_failure_termination(detail::websocket_protocol_failure failure) noexcept {
    switch (failure) {
        case detail::websocket_protocol_failure::protocol_error:
            break;
        case detail::websocket_protocol_failure::invalid_payload_data:
            return websocket_termination::invalid_payload_data;
        case detail::websocket_protocol_failure::message_too_large:
            return websocket_termination::message_too_large;
    }
    return websocket_termination::protocol_error;
}

void add_saturating(std::uint64_t& total, std::size_t bytes_value) noexcept {
    const auto remaining = std::numeric_limits<std::uint64_t>::max() - total;
    total += bytes_value < remaining ? bytes_value : remaining;
}

// Where a data frame sits in its message (RFC 6455 section 5.4): only the
// first frame carries the opcode, only the last one sets FIN.
enum class websocket_fragment_position : std::uint8_t {
    whole,
    first,
    middle,
    last,
};

// UTF-8 of a text message submitted in fragments: a code point may straddle
// fragments, so the bytes of one left incomplete wait for the next fragment.
class websocket_utf8_fragments final {
public:
    [[nodiscard]] bool append(std::string_view fragment) noexcept {
        std::size_t index = 0;
        if (pending_size_ != 0) {
            const auto lead = static_cast<unsigned char>(pending_[0]);
            const std::size_t length = lead >= 0xF0 ? 4 : (lead >= 0xE0 ? 3 : 2);
            while (pending_size_ < length && index < fragment.size()) {
                pending_[pending_size_++] = fragment[index++];
            }
            const auto sequence = scan_utf8_sequence(std::string_view(pending_.data(), pending_size_), 0);
            if (pending_size_ < length) {
                return sequence.length_ == pending_size_;
            }
            if (!sequence.well_formed_) {
                return false;
            }
            pending_size_ = 0;
        }
        while (index < fragment.size()) {
            const auto lead = static_cast<unsigned char>(fragment[index]);
            if (lead < 0x80) {
                ++index;
                continue;
            }
            const auto sequence = scan_utf8_sequence(fragment, index);
            if (sequence.well_formed_) {
                index += sequence.length_;
                continue;
            }
            // A valid lead and continuation bytes cut off by the fragment end.
            if (index + sequence.length_ != fragment.size() || lead < 0xC2 || lead > 0xF4) {
                return false;
            }
            for (; index < fragment.size(); ++index) {
                pending_[pending_size_++] = fragment[index];
            }
        }
        return true;
    }

    [[nodiscard]] bool complete() const noexcept {
        return pending_size_ == 0;
    }

private:
    std::array<char, 4> pending_{};
    std::size_t pending_size_{0};
};

// The data message being submitted as fragments.
struct websocket_outbound_fragments final {
    websocket_opcode opcode_;
    // RSV1 went out on the first frame: every fragment continues one
    // permessage-deflate stream.
    bool compressed_;
    std::size_t size_;
    websocket_utf8_fragments utf8_;
};

}  // namespace

class websocket_connection::impl_type final {
public:
    enum class close_phase_type : std::uint8_t {
        open,
        // A locally initiated Close is at the tail of pending output. Once
        // flushed, the connection keeps receiving until the peer Close.
        local_close_queued,
        awaiting_peer_close,
        // A peer Close was received (or the connection failed) and the final
        // queued output still needs to be flushed before transport end. It may
        // be a Pong queued for an earlier Ping, not another Close.
        final_output_queued,
        // Client only (RFC 6455 section 7.1.1): the close handshake is complete
        // and the final output is still queued; once flushed, the server is
        // expected to close the transport first.
        handshake_output_queued,
        awaiting_peer_eof,
        transport_end_ready,
        closed,
    };

    // A null `input` gives the connection an input buffer of its own.
    impl_type(http_io_buffer* input, websocket_connection_options options) :
        input_(input != nullptr ? input : &owned_input_.emplace()),
        max_buffered_input_bytes_(options.max_buffered_input_bytes_),
        message_limit_(options.message_limit_),
        receive_message_limit_(options.receive_message_limit_.read_ceiling() < options.message_limit_.read_ceiling() ? options.receive_message_limit_ : options.message_limit_),
        role_(options.role_),
        mask_key_generator_(options.mask_key_generator_),
        mask_key_context_(options.mask_key_context_) {
        initialize_codecs(options);
    }

    void initialize_codecs(const websocket_connection_options& options) {
        const auto& compression = options.compression_;
        if (max_buffered_input_bytes_ == 0) {
            throw std::invalid_argument("WebSocket input buffer limit must be greater than zero");
        }
        if (role_ != websocket_connection_role::server && role_ != websocket_connection_role::client) {
            throw std::invalid_argument("invalid WebSocket connection role");
        }
        if (compression.server_max_window_bits_.value_or(15) < 8 || compression.server_max_window_bits_.value_or(15) > 15 || compression.client_max_window_bits_.value_or(15) < 8 || compression.client_max_window_bits_.value_or(15) > 15) {
            throw std::invalid_argument("invalid WebSocket compression window");
        }
        if (role_ == websocket_connection_role::client && mask_key_generator_ == nullptr) {
            throw std::invalid_argument("WebSocket client connection requires a mask key generator");
        }
        validate_websocket_deflater_config(options.deflater_);
        if (compression.enabled_) {
            const bool server = role_ == websocket_connection_role::server;
            const bool send_takeover = !(server ? compression.server_no_context_takeover_ : compression.client_no_context_takeover_);
            const bool receive_takeover = !(server ? compression.client_no_context_takeover_ : compression.server_no_context_takeover_);
            const int send_window = (server ? compression.server_max_window_bits_ : compression.client_max_window_bits_).value_or(15);
            const int receive_window = (server ? compression.client_max_window_bits_ : compression.server_max_window_bits_).value_or(15);
            deflate_.emplace(options.shared_deflate_, options.deflater_, send_takeover, receive_takeover, send_window, receive_window);
        }
    }

    [[nodiscard]] std::size_t pending_input_bytes() const noexcept {
        return input_->size() - pending_consume_;
    }

    void release_consumed_input() noexcept {
        // Keep the storage: the caller may append bytes that alias the released frame.
        input_->consume(std::exchange(pending_consume_, 0));
    }

    [[nodiscard]] websocket_liveness_mode liveness_mode() const noexcept {
        switch (close_phase_) {
            case close_phase_type::open:
                return websocket_liveness_mode::open;
            case close_phase_type::local_close_queued:
            case close_phase_type::awaiting_peer_close:
                return websocket_liveness_mode::awaiting_peer_close;
            case close_phase_type::handshake_output_queued:
            case close_phase_type::awaiting_peer_eof:
                return websocket_liveness_mode::awaiting_peer_eof;
            case close_phase_type::final_output_queued:
            case close_phase_type::transport_end_ready:
            case close_phase_type::closed:
                return websocket_liveness_mode::inactive;
        }
        return websocket_liveness_mode::inactive;
    }

    void append_frame(websocket_opcode opcode, std::string_view payload_value, bool rsv1 = false, websocket_payload_storage storage = websocket_payload_storage::copy, websocket_fragment_position position = websocket_fragment_position::whole) {
        std::optional<std::string> owned_payload;
        if (detail::http_text_overlaps(payload_value, out_buffer_.view())) {
            // A plan may be its own append source. Snapshot it before prepare()
            // can move storage, and never borrow the temporary snapshot.
            payload_value = owned_payload.emplace(payload_value);
            storage = websocket_payload_storage::copy;
        }
        detail::websocket_frame_header_type header;
        const bool masked = role_ == websocket_connection_role::client;
        websocket_mask_key_type mask{};
        if (masked && !mask_key_generator_(mask_key_context_, mask)) {
            throw std::runtime_error("failed to generate WebSocket client mask key");
        }
        const bool borrow = storage == websocket_payload_storage::borrow && !masked;
        const bool continuation = position == websocket_fragment_position::middle || position == websocket_fragment_position::last;
        const bool final = position == websocket_fragment_position::whole || position == websocket_fragment_position::last;
        const auto header_size = detail::encode_websocket_frame_header(header, continuation ? std::uint8_t{0} : static_cast<std::uint8_t>(opcode), final, payload_value.size(), rsv1, masked);
        const auto mask_size = masked ? mask.size() : std::size_t{0};
        std::size_t additional_size = 0;
        for (const auto size : {borrowed_output_.size(), header_size, mask_size, borrow ? 0 : payload_value.size()}) {
            if (size > std::numeric_limits<std::size_t>::max() - additional_size) {
                throw std::length_error("WebSocket output frame size overflow");
            }
            additional_size += size;
        }
        // Prepare every byte before publishing anything. The appends below
        // then fit without allocation, so a failure cannot orphan a wire header.
        (void)out_buffer_.prepare(additional_size);
        // An earlier borrowed payload precedes this frame on the wire.
        out_buffer_.append(borrowed_output_);
        borrowed_output_ = {};
        out_buffer_.append(std::string_view(header.data(), header_size));
        if (masked) {
            out_buffer_.append(std::string_view(mask.data(), mask.size()));
            const auto payload_start = out_buffer_.size();
            out_buffer_.append(payload_value);
            detail::decode_masked_websocket_payload(out_buffer_.data() + payload_start, payload_value.size(), mask.data());
        } else if (borrow) {
            borrowed_output_ = payload_value;
        } else {
            out_buffer_.append(payload_value);
        }
    }

    void fail(std::uint16_t code) {
        if (close_phase_ == close_phase_type::open) {
            // Every protocol failure code is a valid Close code, so encoding succeeds.
            const auto payload_value = detail::encode_websocket_close_payload(websocket_close_code::from_value(code), {});
            append_frame(websocket_opcode::close, payload_value.encoded()->bytes());
            close_phase_ = close_phase_type::final_output_queued;
            return;
        }
        if (close_phase_ == close_phase_type::local_close_queued) {
            close_phase_ = close_phase_type::final_output_queued;
        } else if (close_phase_ == close_phase_type::awaiting_peer_close) {
            close_phase_ = !out_buffer_.empty() || !borrowed_output_.empty() ? close_phase_type::final_output_queued : close_phase_type::transport_end_ready;
        }
    }

    // Both Close frames have been exchanged once the queued output (which may
    // end with a Close echo) is flushed. A server then ends the transport; a
    // client waits for the server to do so.
    [[nodiscard]] close_phase_type handshake_complete_phase() const noexcept {
        const bool output_queued = !out_buffer_.empty() || !borrowed_output_.empty();
        if (role_ == websocket_connection_role::client) {
            return output_queued ? close_phase_type::handshake_output_queued : close_phase_type::awaiting_peer_eof;
        }
        return output_queued ? close_phase_type::final_output_queued : close_phase_type::transport_end_ready;
    }

    void receive_peer_close() noexcept {
        if (close_phase_ == close_phase_type::local_close_queued || close_phase_ == close_phase_type::awaiting_peer_close) {
            close_phase_ = handshake_complete_phase();
        }
    }

    void discard_input() noexcept {
        pending_consume_ = 0;
        input_->consume(input_->size());
    }

    void record_termination(websocket_termination cause) noexcept {
        if (termination_ == websocket_termination::none) {
            termination_ = cause;
        }
    }

    // The one submission behind both submit_frame() payload forms.
    [[nodiscard]] websocket_frame_submit_status submit_frame(websocket_opcode opcode, std::string_view payload_value, websocket_frame_options options, websocket_payload_storage storage) {
        if (close_phase_ != close_phase_type::open) {
            return websocket_frame_submit_status::not_open;
        }
        if (opcode == websocket_opcode::ping || opcode == websocket_opcode::pong) {
            // Control frames are never fragmented (RFC 6455 section 5.5).
            if (!options.final_) {
                return websocket_frame_submit_status::invalid_opcode;
            }
            if (payload_value.size() > 125) {
                return websocket_frame_submit_status::control_frame_too_large;
            }
            append_frame(opcode, payload_value, false, storage);
            return websocket_frame_submit_status::accepted;
        }
        if (opcode != websocket_opcode::text && opcode != websocket_opcode::binary) {
            return websocket_frame_submit_status::invalid_opcode;
        }
        if (outbound_fragments_.has_value() && outbound_fragments_->opcode_ != opcode) {
            return websocket_frame_submit_status::message_in_progress;
        }
        const bool first = !outbound_fragments_.has_value();
        const bool final = options.final_;
        const std::size_t uncompressed_size = payload_value.size();
        const std::size_t sent = first ? 0 : outbound_fragments_->size_;
        if (message_limit_.addition_exceeds(sent, uncompressed_size)) {
            return websocket_frame_submit_status::message_too_large;
        }
        auto utf8 = first ? websocket_utf8_fragments{} : outbound_fragments_->utf8_;
        if (opcode == websocket_opcode::text && (!utf8.append(payload_value) || (final && !utf8.complete()))) {
            return websocket_frame_submit_status::invalid_text_payload;
        }
        const bool compression_attempted = first ? options.compress_ && deflate_.has_value() : outbound_fragments_->compressed_;
        bool compressed = compression_attempted;
        const auto position = first ? (final ? websocket_fragment_position::whole : websocket_fragment_position::first) : (final ? websocket_fragment_position::last : websocket_fragment_position::middle);
        try {
            if (compressed) {
                outbound_deflated_.clear();
                if (position == websocket_fragment_position::whole) {
                    // A whole message is sent uncompressed unless deflate shrinks it.
                    if (deflate_->compress(payload_value, outbound_deflated_) && outbound_deflated_.size() < payload_value.size()) {
                        payload_value = outbound_deflated_;
                    } else {
                        deflate_->discard_compression();
                        compressed = false;
                    }
                } else if (deflate_->compress_fragment(payload_value, first, final, outbound_deflated_)) {
                    payload_value = outbound_deflated_;
                } else if (first) {
                    // Nothing went out yet: send this message uncompressed.
                    deflate_->discard_compression();
                    compressed = false;
                } else {
                    throw std::runtime_error("failed to compress a WebSocket message fragment");
                }
            }
            // A compressed payload lives in outbound_deflated_, which the next
            // message reuses, so only the caller's own bytes can be borrowed.
            append_frame(opcode, payload_value, first && compressed, compressed ? websocket_payload_storage::copy : storage, position);
        } catch (...) {
            if (compression_attempted) {
                http_clear_string_retaining_small(outbound_deflated_, websocket_retained_buffer_bytes);
            }
            if (compressed) {
                // The peer never received a first frame's trial, so a later
                // message must not refer to its dictionary. Earlier fragments
                // already advanced the peer's inflater, so a later fragment
                // cannot be retried; nor can the first one once resetting its
                // trial fails.
                bool reusable = first;
                if (first) {
                    try {
                        deflate_->discard_compression();
                    } catch (...) {
                        reusable = false;
                    }
                }
                if (!reusable) {
                    close_phase_ = close_phase_type::transport_end_ready;
                    record_termination(websocket_termination::aborted);
                }
            }
            throw;
        }
        if (compression_attempted) {
            // The frame owns a copy now; keep only a bounded scratch.
            http_clear_string_retaining_small(outbound_deflated_, websocket_retained_buffer_bytes);
        }
        if (final) {
            outbound_fragments_.reset();
        } else if (first) {
            outbound_fragments_.emplace(websocket_outbound_fragments{opcode, compressed, uncompressed_size, utf8});
        } else {
            outbound_fragments_->size_ = sent + uncompressed_size;
            outbound_fragments_->utf8_ = utf8;
        }
        return websocket_frame_submit_status::accepted;
    }

    std::optional<http_io_buffer> owned_input_;
    http_io_buffer* input_;
    const std::size_t max_buffered_input_bytes_;
    protocol_byte_limit message_limit_;
    protocol_byte_limit receive_message_limit_;
    // Bytes of the last returned frame, consumed by the next next_event().
    std::size_t pending_consume_{0};
    std::size_t needed_input_bytes_{0};

    http_io_buffer out_buffer_;
    // An unmasked, uncompressed payload that follows out_buffer_ on the wire
    // without being copied into it. Only the newest frame can borrow; queuing
    // more output first appends it to out_buffer_.
    std::string_view borrowed_output_;

    detail::websocket_inbound_assembler assembler_;

    std::optional<detail::websocket_deflate> deflate_;
    std::string inbound_inflated_;
    std::string outbound_deflated_;
    std::optional<websocket_outbound_fragments> outbound_fragments_;

    websocket_connection_role role_{websocket_connection_role::server};
    websocket_mask_key_generator_type mask_key_generator_{nullptr};
    void* mask_key_context_{nullptr};

    close_phase_type close_phase_{close_phase_type::open};
    websocket_termination termination_{websocket_termination::none};
    std::uint64_t bytes_received_{0};
    std::uint64_t bytes_sent_{0};
};

websocket_connection::websocket_connection(websocket_connection_options options) :
    impl_(std::make_unique<impl_type>(nullptr, options)) {}

websocket_connection::websocket_connection(http_io_buffer& input, websocket_connection_options options) :
    impl_(std::make_unique<impl_type>(&input, options)) {}

websocket_connection::~websocket_connection() = default;
websocket_connection::websocket_connection(websocket_connection&&) noexcept = default;
websocket_connection& websocket_connection::operator=(websocket_connection&&) noexcept = default;

websocket_feed_status websocket_connection::feed(std::string_view input) {
    const auto liveness = impl_->liveness_mode();
    if (liveness == websocket_liveness_mode::inactive) {
        return websocket_feed_status::inactive;
    }
    if (liveness == websocket_liveness_mode::awaiting_peer_eof) {
        // Bytes after the close handshake carry nothing; the client keeps
        // reading only to observe the server's EOF.
        return websocket_feed_status::accepted;
    }
    if (impl_->input_->size() > impl_->max_buffered_input_bytes_ || input.size() > impl_->max_buffered_input_bytes_ - impl_->input_->size()) {
        if (impl_->pending_input_bytes() > impl_->max_buffered_input_bytes_ || input.size() > impl_->max_buffered_input_bytes_ - impl_->pending_input_bytes()) {
            return websocket_feed_status::backpressured;
        }
        // The bound covers bytes awaiting next_event(), not the frame already
        // returned; otherwise a frame that fits could never arrive. append()
        // copies an `input` aliasing the buffer before it moves any byte.
        impl_->release_consumed_input();
    }
    impl_->input_->append(input);
    return websocket_feed_status::accepted;
}

std::optional<websocket_event> websocket_connection::next_event() & {
    using close_phase_type = impl_type::close_phase_type;
    try {
        impl_->assembler_.release_completed();
        // The last delivered message expires now. Its size is peer-controlled up
        // to the message limit, so only a bounded buffer stays for the next one.
        http_clear_string_retaining_small(impl_->inbound_inflated_, websocket_retained_buffer_bytes);
        impl_->needed_input_bytes_ = 0;
        if (impl_->close_phase_ == close_phase_type::final_output_queued || impl_->close_phase_ == close_phase_type::transport_end_ready || impl_->close_phase_ == close_phase_type::closed) {
            return websocket_event::make_transport_end();
        }
        if (impl_->close_phase_ == close_phase_type::handshake_output_queued) {
            // Queued output may still borrow input bytes: drop them once flushed.
            return std::nullopt;
        }
        if (impl_->close_phase_ == close_phase_type::awaiting_peer_eof) {
            impl_->discard_input();
            return std::nullopt;
        }

        const auto protocol_failure_event = [this](detail::websocket_protocol_failure failure) {
            const auto close_code = detail::websocket_protocol_failure_close_code(failure);
            impl_->fail(close_code);
            impl_->record_termination(websocket_failure_termination(failure));
            return websocket_event::protocol_error(close_code);
        };

        for (;;) {
            const auto read = detail::websocket_try_read_frame(*impl_->input_, impl_->pending_consume_, impl_->receive_message_limit_, impl_->deflate_.has_value(), impl_->role_ == websocket_connection_role::server);
            if (const auto* need_input = read.need_input()) {
                impl_->needed_input_bytes_ = need_input->missing_bytes();
                return std::nullopt;
            }
            if (const auto* failure = read.failure()) {
                return protocol_failure_event(failure->error());
            }

            add_saturating(impl_->bytes_received_, impl_->pending_consume_);
            const auto& frame = *read.frame();
            const auto inbound = impl_->assembler_.accept(frame, impl_->receive_message_limit_);
            if (const auto* failure = inbound.failure()) {
                return protocol_failure_event(failure->error());
            }
            if (inbound.continue_reading() != nullptr) {
                continue;
            }
            if (const auto* control = inbound.control_frame()) {
                const auto payload_value = control->payload();
                if (control->opcode() == websocket_opcode::ping) {
                    // RFC 6455 requires Pong until a peer Close has arrived. A Pong
                    // is a control frame, so it remains legal while a locally
                    // initiated Close waits for its peer response.
                    impl_->append_frame(websocket_opcode::pong, payload_value);
                    return websocket_event::ping(payload_value);
                }
                if (control->opcode() == websocket_opcode::pong) {
                    return websocket_event::pong(payload_value);
                }
                // Close: the frame reader admits no other control opcode.
                std::uint16_t code = 1005;
                if (payload_value.size() >= 2) {
                    code = detail::read_websocket_uint16(payload_value.data());
                }
                const auto reason = payload_value.size() > 2 ? payload_value.substr(2) : std::string_view{};
                if (impl_->close_phase_ == close_phase_type::open) {
                    impl_->append_frame(websocket_opcode::close, payload_value);
                    impl_->close_phase_ = impl_->handshake_complete_phase();
                    impl_->record_termination(websocket_termination::peer_close);
                } else {
                    impl_->receive_peer_close();
                    impl_->record_termination(websocket_termination::local_close);
                }
                return websocket_event::close(code, reason);
            }

            const auto& inbound_message = *inbound.message();
            const auto& message = inbound_message.message();
            if (inbound_message.content_encoding() == detail::websocket_inbound_content_encoding::identity) {
                if (impl_->close_phase_ != close_phase_type::open) {
                    impl_->assembler_.release_completed();
                    continue;
                }
                return websocket_event::message(message.opcode(), message.payload());
            }

            // decompress() only appends, so the buffer must be emptied per MESSAGE, not
            // per next_event(): one next_event() drains several frames, and a message suppressed
            // during the closing handshake (below) returns via `continue` with its bytes
            // still here. Inheriting them would make the next message's UTF-8 check read
            // the concatenation, and would charge its decompression-bomb limit for both.
            impl_->inbound_inflated_.clear();
            // Only a negotiated deflate admits RSV1, so a compressed message implies deflate_.
            const auto inflate_result = impl_->deflate_->decompress(message.payload(), impl_->inbound_inflated_, impl_->receive_message_limit_);
            if (inflate_result == websocket_inflate_result::too_large) {
                return protocol_failure_event(detail::websocket_protocol_failure::message_too_large);
            }
            if (inflate_result != websocket_inflate_result::ok) {
                return protocol_failure_event(detail::websocket_protocol_failure::protocol_error);
            }
            const std::string_view view = impl_->inbound_inflated_;
            if (message.opcode() == websocket_opcode::text && !is_valid_utf8(view)) {
                return protocol_failure_event(detail::websocket_protocol_failure::invalid_payload_data);
            }
            if (impl_->close_phase_ != close_phase_type::open) {
                impl_->assembler_.release_completed();
                http_clear_string_retaining_small(impl_->inbound_inflated_, websocket_retained_buffer_bytes);
                continue;
            }
            return websocket_event::message(message.opcode(), view);
        }
    } catch (...) {
        // Frame reading unmasks in place and advances the input cursor. If any
        // later assembler/deflate/output operation fails, retrying that frame is
        // no longer well-defined. The transport has not ended yet, so the plan
        // and abort() must still tell the driver to end it, as after EOF.
        if (impl_->close_phase_ != close_phase_type::closed) {
            impl_->close_phase_ = close_phase_type::transport_end_ready;
        }
        impl_->record_termination(websocket_termination::aborted);
        throw;
    }
}

std::size_t websocket_connection::needed_input_bytes() const noexcept {
    return impl_->needed_input_bytes_;
}

websocket_output_plan websocket_connection::output_plan() const& noexcept {
    using close_phase_type = impl_type::close_phase_type;
    // EOF/abort may race an async transport write. Keep the backing allocation
    // untouched until destruction, but make discarded bytes unreachable from the
    // protocol driver once transport termination has become authoritative.
    if (impl_->close_phase_ == close_phase_type::transport_end_ready || impl_->close_phase_ == close_phase_type::closed) {
        return websocket_output_plan({}, {}, impl_->close_phase_ == close_phase_type::transport_end_ready ? websocket_transport_disposition::end_transport : websocket_transport_disposition::keep_open);
    }
    const auto bytes_value = impl_->out_buffer_.view();
    const auto disposition = impl_->close_phase_ == close_phase_type::final_output_queued ? websocket_transport_disposition::end_transport : websocket_transport_disposition::keep_open;
    return websocket_output_plan(bytes_value, impl_->borrowed_output_, disposition);
}

websocket_output_consume_status websocket_connection::consume_output(std::size_t bytes_value) noexcept {
    using close_phase_type = impl_type::close_phase_type;
    // EOF/abort makes unsent bytes unreachable without clearing their storage:
    // an async write may still borrow it. A zero-byte transport-end write has
    // no output left to consume, regardless of those discarded backing bytes.
    if (bytes_value == 0 && (impl_->close_phase_ == close_phase_type::transport_end_ready || impl_->close_phase_ == close_phase_type::closed)) {
        return websocket_output_consume_status::drained;
    }
    const auto owned = impl_->out_buffer_.size();
    if (bytes_value > owned && bytes_value - owned > impl_->borrowed_output_.size()) {
        return websocket_output_consume_status::out_of_range;
    }
    add_saturating(impl_->bytes_sent_, bytes_value);
    if (bytes_value < owned) {
        impl_->out_buffer_.consume(bytes_value);
        return websocket_output_consume_status::pending;
    }
    // The transport has written every owned byte, so a large frame's storage
    // can go back to the resource instead of staying with the connection.
    impl_->out_buffer_.clear_retaining_small(websocket_retained_buffer_bytes);
    if (bytes_value - owned < impl_->borrowed_output_.size()) {
        impl_->borrowed_output_.remove_prefix(bytes_value - owned);
        return websocket_output_consume_status::pending;
    }
    impl_->borrowed_output_ = {};
    if (impl_->close_phase_ == close_phase_type::local_close_queued) {
        impl_->close_phase_ = close_phase_type::awaiting_peer_close;
    } else if (impl_->close_phase_ == close_phase_type::final_output_queued) {
        impl_->close_phase_ = close_phase_type::transport_end_ready;
    } else if (impl_->close_phase_ == close_phase_type::handshake_output_queued) {
        impl_->close_phase_ = close_phase_type::awaiting_peer_eof;
    }
    return websocket_output_consume_status::drained;
}

void websocket_connection::commit_transport_end() noexcept {
    if (impl_->close_phase_ == impl_type::close_phase_type::transport_end_ready) {
        impl_->close_phase_ = impl_type::close_phase_type::closed;
    }
}

void websocket_connection::notify_transport_eof() noexcept {
    if (impl_->close_phase_ != impl_type::close_phase_type::closed) {
        impl_->close_phase_ = impl_type::close_phase_type::transport_end_ready;
    }
    impl_->record_termination(websocket_termination::eof);
}

websocket_abort_disposition websocket_connection::abort(websocket_termination cause) noexcept {
    impl_->record_termination(cause == websocket_termination::none ? websocket_termination::aborted : cause);
    if (impl_->close_phase_ == impl_type::close_phase_type::closed) {
        return websocket_abort_disposition::no_transport_action;
    }
    impl_->close_phase_ = impl_type::close_phase_type::closed;
    return websocket_abort_disposition::abort_transport;
}

websocket_liveness_mode websocket_connection::liveness_mode() const noexcept {
    return impl_->liveness_mode();
}

websocket_termination websocket_connection::termination() const noexcept {
    return impl_->termination_;
}

std::uint64_t websocket_connection::bytes_received() const noexcept {
    return impl_->bytes_received_;
}

std::uint64_t websocket_connection::bytes_sent() const noexcept {
    return impl_->bytes_sent_;
}

websocket_frame_submit_status websocket_connection::submit_frame(websocket_opcode opcode, std::string_view payload_value, websocket_frame_options options) {
    return impl_->submit_frame(opcode, payload_value, options, websocket_payload_storage::copy);
}

websocket_frame_submit_status websocket_connection::submit_frame(websocket_opcode opcode, websocket_borrowed_payload payload_value, websocket_frame_options options) {
    return impl_->submit_frame(opcode, payload_value.bytes(), options, websocket_payload_storage::borrow);
}

websocket_close_submit_status websocket_connection::submit_close(websocket_close_code code, std::string_view reason) {
    using close_phase_type = impl_type::close_phase_type;
    if (impl_->close_phase_ == close_phase_type::closed) {
        return websocket_close_submit_status::closed;
    }
    if (impl_->close_phase_ != close_phase_type::open) {
        return websocket_close_submit_status::already_closing;
    }
    // RFC 6455 §7.4.1 reserves 1010 for a client reporting extensions that
    // were absent from the server handshake. This core emits server frames;
    // a server must reject that mismatch during the opening handshake rather
    // than initiate a Close frame with the client-only status code.
    if (impl_->role_ == websocket_connection_role::server && code.value() == 1010) {
        return websocket_close_submit_status::invalid_code;
    }
    const auto payload_value = detail::encode_websocket_close_payload(code, reason);
    if (const auto* failure = payload_value.failure()) {
        switch (failure->error()) {
            case detail::websocket_close_payload_encode_error::invalid_reason:
                return websocket_close_submit_status::invalid_reason;
            case detail::websocket_close_payload_encode_error::reason_too_large:
                return websocket_close_submit_status::reason_too_large;
        }
    }
    impl_->append_frame(websocket_opcode::close, payload_value.encoded()->bytes());
    impl_->close_phase_ = close_phase_type::local_close_queued;
    return websocket_close_submit_status::accepted;
}

}  // namespace ruvia

namespace ruvia {

websocket_deflate_codecs::~websocket_deflate_codecs() {
    for (auto* codec : deflaters_) {
        delete codec;
    }
    delete inflater_;
}

bool websocket_deflate_codecs::deflate_message(std::string_view payload, std::string& out, int compression_level, int window_bits) {
    if (window_bits < min_window_bits || window_bits > max_window_bits) {
        throw std::invalid_argument("invalid WebSocket DEFLATE window");
    }
    if (compression_level < 0 || compression_level >= compression_levels) {
        throw std::invalid_argument("WebSocket compression level must be between 0 and 9");
    }
    auto& slot = deflaters_[static_cast<std::size_t>((window_bits - min_window_bits) * compression_levels + compression_level)];
    auto* codec = detail::acquire_codec(slot, compression_level, detail::websocket_deflate_zlib_window_bits(window_bits), detail::zlib_default_mem_level, detail::websocket_deflate_zlib_strategy(window_bits));
    if (codec == nullptr) {
        return false;
    }
    const detail::codec_reset_guard reset_guard(slot);
    return detail::websocket_deflate_message(*codec, payload, out);
}

websocket_inflate_result websocket_deflate_codecs::inflate_message(std::string_view payload, std::string& out, protocol_byte_limit message_limit) {
    auto* codec = detail::acquire_codec(inflater_, -max_window_bits);
    if (codec == nullptr) {
        return websocket_inflate_result::error;
    }
    const detail::codec_reset_guard reset_guard(inflater_);
    return detail::websocket_inflate_message(*codec, payload, out, message_limit);
}

}  // namespace ruvia

namespace ruvia::detail {
namespace {

[[nodiscard]] std::vector<http_header> copy_websocket_response_headers(std::span<const http_header_view> headers, std::string_view subprotocol, std::string_view extension) {
    // Account for the whole emitted head: the fixed 101 prefix, the accept
    // key line, the negotiated and application fields, and the terminating
    // empty line.
    http_field_section_budget size;
    (void)size.add(websocket_switching_protocols_head_prefix.size() + websocket_accept_key_type{}.size() + std::string_view("\r\n").size());
    (void)size.add(std::string_view("\r\n").size());
    if (!subprotocol.empty() && !size.add_field("sec-websocket-protocol", subprotocol)) {
        throw std::length_error("WebSocket handshake response headers are too large");
    }
    if (!extension.empty() && !size.add_field("sec-websocket-extensions", extension)) {
        throw std::length_error("WebSocket handshake response headers are too large");
    }
    static constexpr std::string_view sec_websocket_prefix = "sec-websocket-";
    for (const auto& header : headers) {
        const auto name = header.name();
        if (!is_valid_http_header_name(name) || !is_valid_http_header_value(header.value())) {
            throw std::invalid_argument("invalid WebSocket handshake response header");
        }
        // Every Sec-WebSocket-* name is handshake-owned, including ones this
        // implementation does not know yet.
        const auto kind = classify_http_field(name);
        const bool is_sec_websocket = name.size() >= sec_websocket_prefix.size() && http_ascii_equals_ignore_case(name.substr(0, sec_websocket_prefix.size()), sec_websocket_prefix);
        if ((kind != http_field_kind::host && kind != http_field_kind::expect && is_protocol_managed_request_field(kind, name)) || is_sec_websocket) {
            throw std::invalid_argument("WebSocket handshake controls this response header");
        }
        if (!size.add_field(name, header.value())) {
            throw std::length_error("WebSocket handshake response headers are too large");
        }
    }
    std::vector<http_header> result;
    result.reserve(headers.size());
    for (const auto& header : headers) {
        result.push_back(http_header::copy_with_lowercase_name(header.name(), header.value()));
    }
    return result;
}

void skip_websocket_extension_ows(std::string_view value, std::size_t& cursor_value) noexcept {
    while (cursor_value < value.size() && (value[cursor_value] == ' ' || value[cursor_value] == '\t')) {
        ++cursor_value;
    }
}

[[nodiscard]] bool consume_websocket_extension_token(std::string_view value, std::size_t& cursor_value) noexcept {
    const auto start = cursor_value;
    while (cursor_value < value.size() && is_http_token_char(static_cast<unsigned char>(value[cursor_value]))) {
        ++cursor_value;
    }
    return cursor_value != start;
}

// RFC 6455 section 4.3 narrows quoted-string extension values: after
// quoted-pair unescaping, the result still has to be a nonempty token.
[[nodiscard]] bool consume_websocket_extension_quoted_token(std::string_view value, std::size_t& cursor_value) noexcept {
    http_quoted_string_cursor quoted(value.substr(cursor_value));
    bool decoded_any = false;
    unsigned char byte = 0;
    while (quoted.next(byte)) {
        if (!is_http_token_char(byte)) {
            return false;
        }
        decoded_any = true;
    }
    if (!quoted.closed()) {
        return false;
    }
    cursor_value += quoted.consumed();
    return decoded_any;
}

}  // namespace

bool append_websocket_extension_list(std::string_view value, bool& has_extension) noexcept {
    std::size_t cursor_value = 0;
    while (true) {
        skip_websocket_extension_ows(value, cursor_value);
        // RFC 2616's #rule permits null comma-list elements, but 1#extension
        // still requires at least one real extension across the logical field.
        while (cursor_value < value.size() && value[cursor_value] == ',') {
            ++cursor_value;
            skip_websocket_extension_ows(value, cursor_value);
        }
        if (cursor_value == value.size()) {
            return true;
        }
        if (!consume_websocket_extension_token(value, cursor_value)) {
            return false;
        }
        has_extension = true;

        while (true) {
            skip_websocket_extension_ows(value, cursor_value);
            if (cursor_value == value.size()) {
                return true;
            }
            if (value[cursor_value] == ',') {
                ++cursor_value;
                break;
            }
            if (value[cursor_value] != ';') {
                return false;
            }
            ++cursor_value;
            skip_websocket_extension_ows(value, cursor_value);
            if (!consume_websocket_extension_token(value, cursor_value)) {
                return false;
            }
            skip_websocket_extension_ows(value, cursor_value);
            if (cursor_value == value.size() || value[cursor_value] != '=') {
                continue;
            }
            ++cursor_value;
            skip_websocket_extension_ows(value, cursor_value);
            if (cursor_value == value.size()) {
                return false;
            }
            if (value[cursor_value] == '"') {
                if (!consume_websocket_extension_quoted_token(value, cursor_value)) {
                    return false;
                }
            } else if (!consume_websocket_extension_token(value, cursor_value)) {
                return false;
            }
        }
    }
}

std::string_view choose_websocket_subprotocol(const websocket_subprotocol_set& offered, std::span<const std::string_view> supported) noexcept {
    websocket_subprotocol_set configured;
    for (const auto protocol : supported) {
        if (!configured.append(protocol)) {
            return {};
        }
    }
    if (offered.empty()) {
        return {};
    }
    for (const auto protocol : supported) {
        if (offered.contains(protocol)) {
            return protocol;
        }
    }
    return {};
}

std::string_view choose_websocket_subprotocol(const http_request& request, std::span<const std::string_view> supported) noexcept {
    if (supported.empty()) {
        return {};
    }
    websocket_subprotocol_set offered;
    if (!http_request_access::visit_known_headers(request, http_field_kind::sec_websocket_protocol, [&offered](std::string_view value) noexcept {
        return offered.append_offered_list(value);
    })) {
        return {};
    }
    return choose_websocket_subprotocol(offered, supported);
}

bool websocket_request_header_validator::observe_header(http_field_kind kind, std::string_view value) noexcept {
    if (kind == http_field_kind::sec_websocket_version) {
        version_ = value;
        if (version_present_) {
            valid_ = false;
        }
        version_present_ = true;
    } else if (kind == http_field_kind::sec_websocket_protocol) {
        protocols_present_ = true;
        if (valid_ && !(role_ == http_field_list_role::recipient ? protocols_.append_offered_list(value) : protocols_.append_list(value))) {
            valid_ = false;
        }
    } else if (kind == http_field_kind::sec_websocket_extensions) {
        extensions_present_ = true;
        if (valid_ && !append_websocket_extension_list(value, has_extension_)) {
            valid_ = false;
        }
    }
    return valid_;
}

bool websocket_request_header_validator::offers_valid() const noexcept {
    return valid_ && version_present_ && (!protocols_present_ || !protocols_.empty()) && (!extensions_present_ || has_extension_);
}

bool websocket_request_header_validator::client_offers_valid() const noexcept {
    return offers_valid() && version_ == "13";
}

websocket_handshake_validation_result websocket_request_header_validator::finish(websocket_handshake_validation_options options) const noexcept {
    if (!offers_valid()) {
        return websocket_handshake_validation_result::make_invalid_request();
    }
    if (http_trim_ows(version_) != "13") {
        return websocket_handshake_validation_result::make_unsupported_version();
    }
    if (options.require_subprotocol_ && choose_websocket_subprotocol(protocols_, options.supported_subprotocols_).empty()) {
        return websocket_handshake_validation_result::make_unsupported_subprotocol();
    }
    return websocket_handshake_validation_result::make_accepted();
}

websocket_server_negotiation::websocket_server_negotiation(std::string_view subprotocol, websocket_compression compression, std::span<const http_header_view> response_headers_value) :
    subprotocol_(subprotocol),
    compression_(compression),
    extensions_(compression),
    response_headers_(copy_websocket_response_headers(response_headers_value, subprotocol, extensions_.view())) {}

websocket_server_negotiation make_websocket_server_negotiation(const http_request& request, const websocket_server_handshake_options& options) {
    return websocket_server_negotiation(choose_websocket_subprotocol(request, options.supported_subprotocols_), websocket_negotiate_permessage_deflate(request, options.deflate_), options.response_headers_);
}

websocket_handshake_validation_result validate_extended_connect_websocket_headers(const http_request& request, websocket_handshake_validation_options options) noexcept {
    websocket_request_header_validator validator(http_field_list_role::recipient);
    const auto headers = request.headers();
    for (std::size_t index = 0; index < headers.size(); ++index) {
        if (!validator.observe_header(http_request_access::header_kind(request, index), headers[index].value())) {
            return validator.finish(options);
        }
    }
    return validator.finish(options);
}

}  // namespace ruvia::detail

namespace ruvia {

websocket_server_handshake make_websocket_server_handshake(const http_request& request, websocket_server_handshake_options options) {
    detail::websocket_accept_key_type accept;
    detail::encode_websocket_accept(accept, detail::http_request_access::known_header(request, detail::http_field_kind::sec_websocket_key));
    auto negotiation = detail::make_websocket_server_negotiation(request, options);
    return websocket_server_handshake(accept, std::move(negotiation).take_subprotocol(), negotiation.compression(), std::move(negotiation).take_response_headers());
}

}  // namespace ruvia

namespace ruvia::detail {
namespace {

constexpr std::string_view websocket_guid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
constexpr std::uint64_t max_sha1_message_bytes = (std::numeric_limits<std::uint64_t>::max)() / 8;

[[nodiscard]] std::array<std::uint8_t, 20> sha1(std::string_view first, std::string_view second) noexcept {
    std::uint32_t h0 = 0x67452301U;
    std::uint32_t h1 = 0xEFCDAB89U;
    std::uint32_t h2 = 0x98BADCFEU;
    std::uint32_t h3 = 0x10325476U;
    std::uint32_t h4 = 0xC3D2E1F0U;
    std::array<std::uint8_t, 64> block{};
    const auto total_size = first.size() + second.size();
    const std::uint64_t total_bits = static_cast<std::uint64_t>(total_size) * 8U;
    std::size_t offset = 0;

    const auto byte_at = [first, second](std::size_t index) noexcept {
        return index < first.size() ? static_cast<std::uint8_t>(first[index]) : static_cast<std::uint8_t>(second[index - first.size()]);
    };

    const auto process = [&](const std::array<std::uint8_t, 64>& data) noexcept {
        std::array<std::uint32_t, 80> w{};
        for (std::size_t i = 0; i < 16; ++i) {
            w[i] = (static_cast<std::uint32_t>(data[i * 4]) << 24) | (static_cast<std::uint32_t>(data[i * 4 + 1]) << 16) | (static_cast<std::uint32_t>(data[i * 4 + 2]) << 8) | static_cast<std::uint32_t>(data[i * 4 + 3]);
        }
        for (std::size_t i = 16; i < 80; ++i) {
            w[i] = std::rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }
        auto a = h0;
        auto b = h1;
        auto c = h2;
        auto d = h3;
        auto e = h4;
        for (std::size_t i = 0; i < 80; ++i) {
            std::uint32_t f = 0;
            std::uint32_t k = 0;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999U;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1U;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDCU;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6U;
            }
            const auto temp = std::rotl(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = std::rotl(b, 30);
            b = a;
            a = temp;
        }
        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
        h4 += e;
    };

    while (total_size - offset >= block.size()) {
        for (std::size_t i = 0; i < block.size(); ++i) {
            block[i] = byte_at(offset + i);
        }
        process(block);
        offset += block.size();
    }
    block.fill(0);
    const auto remaining = total_size - offset;
    for (std::size_t i = 0; i < remaining; ++i) {
        block[i] = byte_at(offset + i);
    }
    block[remaining] = 0x80;
    if (remaining >= 56) {
        process(block);
        block.fill(0);
    }
    for (std::size_t i = 0; i < 8; ++i) {
        block[63 - i] = static_cast<std::uint8_t>((total_bits >> (i * 8)) & 0xFF);
    }
    process(block);

    std::array<std::uint8_t, 20> digest{};
    const std::array words{h0, h1, h2, h3, h4};
    for (std::size_t i = 0; i < words.size(); ++i) {
        digest[i * 4] = static_cast<std::uint8_t>((words[i] >> 24) & 0xFF);
        digest[i * 4 + 1] = static_cast<std::uint8_t>((words[i] >> 16) & 0xFF);
        digest[i * 4 + 2] = static_cast<std::uint8_t>((words[i] >> 8) & 0xFF);
        digest[i * 4 + 3] = static_cast<std::uint8_t>(words[i] & 0xFF);
    }
    return digest;
}

}  // namespace

void encode_websocket_accept(websocket_accept_key_type& output, std::string_view key) {
    // Check the raw view before trimming: http_trim_ows() must inspect the last
    // byte, and callers of this detail helper are not otherwise required to
    // prove that a view's advertised size is backed by a full object. The
    // SHA-1 length field is measured in bits and is only 64 bits wide.
    if (key.size() > (std::numeric_limits<std::size_t>::max)() - websocket_guid.size() || key.size() > max_sha1_message_bytes - websocket_guid.size()) {
        throw std::length_error("WebSocket accept input is too large");
    }
    key = http_trim_ows(key);
    const auto digest = sha1(key, websocket_guid);
    http_base64_encode(output.data(), std::span<const std::uint8_t>(digest));
}

}  // namespace ruvia::detail

namespace ruvia::detail {
namespace {

// A Sec-WebSocket-Key is the canonical padded base64 of a 16-byte nonce
// (RFC 6455 section 4.1), so exactly 24 characters.
[[nodiscard]] std::optional<std::array<std::uint8_t, 16>> decode_websocket_key(std::string_view key) noexcept {
    key = http_trim_ows(key);
    std::array<std::uint8_t, 16> nonce{};
    if (key.size() != 24) {
        return std::nullopt;
    }
    const auto decoded = http_base64_decode(key, nonce, http_base64_charset::standard, http_base64_padding::required);
    if (!decoded || *decoded != nonce.size()) {
        return std::nullopt;
    }
    return nonce;
}

}  // namespace

}  // namespace ruvia::detail

namespace ruvia {

http_protocol_error websocket_handshake_failure::protocol_error() const noexcept {
    switch (kind_) {
        case kind_type::invalid_request:
            return http_protocol_error(http_status::bad_request, "invalid_websocket_handshake", "invalid WebSocket handshake");
        case kind_type::unsupported_version:
            // RFC 6455 section 4.4 requires Sec-websocket-Version and uses 400
            // in its example. 426 would instead require HTTP Upgrade fields.
            return http_protocol_error(http_status::bad_request, "unsupported_websocket_version", "unsupported WebSocket version");
        case kind_type::unsupported_subprotocol:
            return http_protocol_error(http_status::bad_request, "unsupported_websocket_subprotocol", "no supported WebSocket subprotocol offered");
    }
    return http_protocol_error(http_status::bad_request, "invalid_websocket_handshake", "invalid WebSocket handshake");
}

void websocket_handshake_failure::apply_required_response_headers(http_response& response) const {
    if (kind_ == kind_type::unsupported_version) {
        response.header_stable_view("Sec-WebSocket-Version", "13");
    }
}

websocket_handshake_validation_result validate_websocket_handshake(const http_request& request, const http1_request_body_plan& body_plan, websocket_handshake_validation_options options) noexcept {
    if (request.known_method() != http_known_method::get || request.protocol_version() != http_protocol_version::http11 ||
        // RFC 6455 does not forbid Content-Length on the HTTP Upgrade request.
        // The parser-owned framing plan is the authoritative distinction:
        // Content-Length: 0 carries no content, while a positive length or
        // chunked coding still has bytes that must be consumed before the
        // connection can change protocols.
        body_plan.requires_consumption()) {
        return websocket_handshake_validation_result::make_invalid_request();
    }

    detail::http_connection_options connection_options;
    detail::http_upgrade_protocols upgrade_protocols;
    detail::websocket_request_header_validator websocket_headers(detail::http_field_list_role::recipient);
    std::string_view key;
    std::size_t key_count = 0;
    bool websocket_upgrade = false;

    const auto headers = request.headers();
    for (std::size_t i = 0; i < headers.size(); ++i) {
        const auto kind = detail::http_request_access::header_kind(request, i);
        const auto& header_value = headers[i];
        if (kind == detail::http_field_kind::connection) {
            if (connection_options.parse_field(header_value.value(), detail::http_connection_field_role::request_recipient) != detail::http_field_list_parse_status::ok) {
                return websocket_handshake_validation_result::make_invalid_request();
            }
        } else if (kind == detail::http_field_kind::upgrade) {
            if (upgrade_protocols.parse_field(header_value.value(), detail::http_field_list_role::recipient, [&websocket_upgrade](const detail::http_upgrade_protocol& protocol) noexcept {
                if (protocol.version_.empty() && http_ascii_equals_ignore_case(protocol.name_, "websocket")) {
                    websocket_upgrade = true;
                }
                return true;
            }) != detail::http_field_list_parse_status::ok) {
                return websocket_handshake_validation_result::make_invalid_request();
            }
        } else if (kind == detail::http_field_kind::sec_websocket_key) {
            key = header_value.value();
            ++key_count;
        } else if (!websocket_headers.observe_header(kind, header_value.value())) {
            return websocket_handshake_validation_result::make_invalid_request();
        }
    }

    if (!connection_options.upgrade() || !upgrade_protocols.has_protocol() || !websocket_upgrade || key_count != 1 || !detail::decode_websocket_key(key).has_value()) {
        return websocket_handshake_validation_result::make_invalid_request();
    }
    return websocket_headers.finish(options);
}

}  // namespace ruvia

namespace ruvia {
namespace {
using error_type = websocket_client_handshake_error;
std::optional<websocket_compression> parse_response_extension(std::string_view text, const websocket_client_deflate_offer& deflate) {
    const auto& offer = deflate.compression_;
    if (!offer.enabled_) {
        return {};
    }
    const auto params = detail::parse_permessage_deflate_params(text);
    if (!params.has_value()) {
        return {};
    }
    // A response must give client_max_window_bits a value, and may only send it
    // when the offer included it (RFC 7692 section 7.1.2.2).
    if (params->client_max_window_bits_present_ && (!params->client_max_window_bits_.has_value() || (!deflate.offer_client_max_window_bits_ && !offer.client_max_window_bits_))) {
        return {};
    }
    websocket_compression result_value{
        .enabled_ = true,
        .server_no_context_takeover_ = params->server_no_context_takeover_,
        // The client's own takeover opt-out binds its deflater even when the server does not echo it.
        .client_no_context_takeover_ = offer.client_no_context_takeover_ || params->client_no_context_takeover_,
        .server_max_window_bits_ = params->server_max_window_bits_,
        .client_max_window_bits_ = params->client_max_window_bits_
    };
    if (offer.server_no_context_takeover_ && !result_value.server_no_context_takeover_) {
        return {};
    }
    if (offer.server_max_window_bits_ && (!result_value.server_max_window_bits_ || *result_value.server_max_window_bits_ > *offer.server_max_window_bits_)) {
        return {};
    }
    // An offered client window binds this client even when the response omits
    // it or selects a larger one (RFC 7692 section 7.1.2.2).
    if (offer.client_max_window_bits_ && (!result_value.client_max_window_bits_ || *result_value.client_max_window_bits_ > *offer.client_max_window_bits_)) {
        result_value.client_max_window_bits_ = offer.client_max_window_bits_;
    }

    return result_value;
}
[[nodiscard]] bool subprotocol_offered(std::string_view offered_list, std::string_view selected) noexcept {
    if (selected.empty()) {
        return false;
    }
    return !detail::http_find_header_token(offered_list, [selected](std::string_view token) noexcept {
        return token == selected;
    }).empty();
}

std::variant<websocket_client_negotiation_result_view, error_type> validate(std::span<const http_header> fields_value, std::string_view offered_subprotocols, const websocket_client_deflate_offer& offer, bool extended) {
    websocket_client_negotiation_result_view result;
    std::size_t selected = 0;
    bool extensions_present = false;
    bool saw_extension = false;
    for (const auto& field : fields_value) {
        const auto kind = detail::classify_http_field(field.name());
        if (kind == detail::http_field_kind::sec_websocket_protocol) {
            ++selected;
            result.selected_subprotocol_ = http_trim_ows(field.value());
            if (selected > 1 || !subprotocol_offered(offered_subprotocols, result.selected_subprotocol_)) {
                return error_type::subprotocol;
            }
        } else if (kind == detail::http_field_kind::sec_websocket_extensions) {
            extensions_present = true;
            bool valid_line = true;
            http_visit_comma_separated_quoted_field_items(field.value(), [&](std::string_view item) noexcept {
                if (item.empty()) {
                    return true;
                }
                saw_extension = true;
                if (result.compression_.enabled_) {
                    valid_line = false;
                    return false;
                }
                auto negotiation = parse_response_extension(item, offer);
                if (!negotiation) {
                    valid_line = false;
                    return false;
                }
                result.compression_ = *negotiation;
                return true;
            });
            if (!valid_line) {
                return error_type::extensions;
            }
        } else if (extended && detail::is_forbidden_http_binary_response_field(field.name())) {
            // Connection-specific fields are malformed in HTTP/2 (RFC 9113
            // section 8.2.2). Sec-WebSocket-Accept is superseded by :protocol
            // and therefore not processed (RFC 8441 section 5).
            return error_type::forbidden_field;
        }
    }
    if (extensions_present && !saw_extension) {
        return error_type::extensions;
    }
    return result;
}

inline constexpr std::string_view version_header_name = "sec-websocket-version";
inline constexpr std::string_view version_storage = "sec-websocket-version13";
inline constexpr std::string_view protocol_header_name = "sec-websocket-protocol";
inline constexpr std::string_view extensions_header_name = "sec-websocket-extensions";
inline constexpr std::string_view client_window_bits_suffix = "; client_max_window_bits";

[[nodiscard]] const std::string_view* static_deflate_extension_storage(const websocket_client_deflate_offer& deflate) noexcept {
    const auto& offer = deflate.compression_;
    if (!offer.enabled_ || offer.server_max_window_bits_.has_value() || offer.client_max_window_bits_.has_value()) {
        return nullptr;
    }
    static constexpr std::array<std::string_view, 8> table{
        "sec-websocket-extensionspermessage-deflate",
        "sec-websocket-extensionspermessage-deflate; client_max_window_bits",
        "sec-websocket-extensionspermessage-deflate; client_no_context_takeover",
        "sec-websocket-extensionspermessage-deflate; client_no_context_takeover; "
        "client_max_window_bits",
        "sec-websocket-extensionspermessage-deflate; server_no_context_takeover",
        "sec-websocket-extensionspermessage-deflate; server_no_context_takeover; "
        "client_max_window_bits",
        "sec-websocket-extensionspermessage-deflate; server_no_context_takeover; "
        "client_no_context_takeover",
        "sec-websocket-extensionspermessage-deflate; server_no_context_takeover; "
        "client_no_context_takeover; client_max_window_bits",
    };
    const std::size_t index = (offer.server_no_context_takeover_ ? 4U : 0U) | (offer.client_no_context_takeover_ ? 2U : 0U) | (deflate.offer_client_max_window_bits_ ? 1U : 0U);
    return &table[index];
}

// The validated request-field plan of one client configuration.
struct client_offer_plan final {
    // Bytes of the comma-joined Sec-WebSocket-Protocol value; zero without subprotocols.
    std::size_t subprotocol_bytes_{0};
    // Name-prefixed static storage of a parameterless deflate offer.
    const std::string_view* static_extension_{nullptr};
    // A deflate offer carrying window parameters.
    std::optional<detail::websocket_compression_extension> encoded_extension_;
    // The offer lists client_max_window_bits without a value after encoded_extension_.
    bool client_window_bits_suffix_{false};
};

[[nodiscard]] client_offer_plan make_client_offer_plan(const websocket_client_negotiation_config_view& config) {
    const auto& offer = config.deflate_.compression_;
    auto valid_window = [](std::optional<int> window_bits) {
        return !window_bits || (*window_bits >= 8 && *window_bits <= 15);
    };
    if (!valid_window(offer.server_max_window_bits_) || !valid_window(offer.client_max_window_bits_) || (offer.client_max_window_bits_ && !config.deflate_.offer_client_max_window_bits_)) {
        throw std::invalid_argument("invalid WebSocket client deflate offer");
    }
    validate_websocket_deflater_config(config.deflate_.deflater_);
    client_offer_plan plan;
    websocket_subprotocol_set seen;
    for (const auto protocol_view : config.subprotocols_) {
        if (!seen.append(protocol_view)) {
            throw std::invalid_argument("invalid WebSocket subprotocol");
        }
        if (plan.subprotocol_bytes_ != 0) {
            if (plan.subprotocol_bytes_ > max_http_header_bytes - 2) {
                throw std::invalid_argument("WebSocket subprotocol field too large");
            }
            plan.subprotocol_bytes_ += 2;
        }
        if (protocol_view.size() > max_http_header_bytes - plan.subprotocol_bytes_) {
            throw std::invalid_argument("WebSocket subprotocol field too large");
        }
        plan.subprotocol_bytes_ += protocol_view.size();
    }
    // The generated Sec-WebSocket-* fields alone must fit the field section;
    // the handshake owner budgets the application headers it accepts.
    http_field_section_budget budget;
    auto add = [&budget](std::string_view name, std::size_t value_bytes) {
        if (!budget.add_field(name.size(), value_bytes)) {
            throw std::invalid_argument("WebSocket client fields too large");
        }
    };
    add(version_header_name, 2);
    if (plan.subprotocol_bytes_ != 0) {
        add(protocol_header_name, plan.subprotocol_bytes_);
    }
    if (offer.enabled_) {
        plan.static_extension_ = static_deflate_extension_storage(config.deflate_);
        if (plan.static_extension_ != nullptr) {
            add(extensions_header_name, plan.static_extension_->size() - extensions_header_name.size());
        } else {
            const auto& encoded = plan.encoded_extension_.emplace(offer);
            plan.client_window_bits_suffix_ = config.deflate_.offer_client_max_window_bits_ && !offer.client_max_window_bits_;
            add(extensions_header_name, encoded.view().size() + (plan.client_window_bits_suffix_ ? client_window_bits_suffix.size() : 0));
        }
    }
    return plan;
}

[[nodiscard]] http_header make_subprotocol_field(std::span<const std::string_view> subprotocols, std::size_t joined_bytes) {
    if (subprotocols.size() == 1U) {
        return http_header(protocol_header_name, subprotocols.front());
    }
    static constexpr std::size_t inline_joined_capacity = 256;
    if (joined_bytes <= inline_joined_capacity) {
        std::array<char, inline_joined_capacity> joined_buffer;
        std::size_t offset = 0;
        for (const auto protocol_view : subprotocols) {
            if (offset != 0) {
                joined_buffer[offset++] = ',';
                joined_buffer[offset++] = ' ';
            }
            std::memcpy(joined_buffer.data() + offset, protocol_view.data(), protocol_view.size());
            offset += protocol_view.size();
        }
        return http_header(protocol_header_name, std::string_view(joined_buffer.data(), offset));
    }
    std::string joined;
    joined.reserve(joined_bytes);
    for (const auto protocol_view : subprotocols) {
        if (!joined.empty()) {
            joined.append(", ");
        }
        joined.append(protocol_view);
    }
    return http_header(protocol_header_name, joined);
}

// The static offer (a private, storage-borrowing http_header) is built by the
// constructor; this handles the encoded offers that need owned storage.
[[nodiscard]] http_header make_extension_field(const client_offer_plan& plan) {
    const auto encoded = plan.encoded_extension_->view();
    if (!plan.client_window_bits_suffix_) {
        return http_header(extensions_header_name, encoded);
    }
    std::array<char, 128> offer_buffer;
    std::memcpy(offer_buffer.data(), encoded.data(), encoded.size());
    std::memcpy(offer_buffer.data() + encoded.size(), client_window_bits_suffix.data(), client_window_bits_suffix.size());
    return http_header(extensions_header_name, std::string_view(offer_buffer.data(), encoded.size() + client_window_bits_suffix.size()));
}
}  // namespace
void websocket_client_negotiation::validate_configuration(websocket_client_negotiation_config_view config) {
    (void)make_client_offer_plan(config);
}
websocket_client_negotiation::websocket_client_negotiation(websocket_client_negotiation_config_view config) :
    deflate_(config.deflate_) {
    const auto plan = make_client_offer_plan(config);
    fields_[field_count_++] = http_header(version_storage.data(), version_header_name.size(), 2);
    if (plan.subprotocol_bytes_ != 0) {
        fields_[field_count_] = make_subprotocol_field(config.subprotocols_.view(), plan.subprotocol_bytes_);
        offered_subprotocols_ = fields_[field_count_++].value();
    }
    if (config.deflate_.compression_.enabled_) {
        fields_[field_count_++] = plan.static_extension_ != nullptr ? http_header(plan.static_extension_->data(), extensions_header_name.size(), plan.static_extension_->size() - extensions_header_name.size()) : make_extension_field(plan);
    }
}
std::variant<websocket_client_negotiation_result_view, error_type> websocket_client_negotiation::validate_fields(std::span<const http_header> fields_value) const {
    return detail::validate_websocket_client_selection(*this, fields_value, false);
}

std::variant<websocket_client_negotiation_result_view, error_type> validate_extended_connect_websocket_client_response(const websocket_client_negotiation& negotiation, const http_client_response_head& head, bool stream_open) {
    if (!head.status().is_successful()) {
        return error_type::response_status;
    }
    if (!stream_open) {
        return error_type::stream_closed;
    }
    return detail::validate_websocket_client_selection(negotiation, head.headers(), true);
}

namespace detail {
std::variant<websocket_client_negotiation_result_view, error_type> validate_websocket_client_selection(const websocket_client_negotiation& negotiation, std::span<const http_header> fields, bool extended) {
    return validate(fields, negotiation.offered_subprotocols_, negotiation.deflate_, extended);
}
}  // namespace detail
}  // namespace ruvia
