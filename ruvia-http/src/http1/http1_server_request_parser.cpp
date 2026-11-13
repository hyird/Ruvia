#include "ruvia/http/http1_server_request_parser.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>
#include <variant>

#include "ruvia/http/http_field_section.h"
#include "ruvia/http/http_field_syntax.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_io_buffer.h"
#include "ruvia/http/http_number_format.h"
#include "ruvia/http/http_protocol_error.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_response_server.h"
#include "ruvia/http/http_status.h"

#include "http1/http_header_block_parser.h"
#include "http_connection_fields.h"
#include "http_websocket_permessage_deflate.h"
#include "http_wire_sink.h"
#include "http_zlib_stream.h"

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
[[nodiscard]] http_parse_error classify_head_cap(std::string_view buffer, std::size_t leading_bytes,
    std::size_t request_line_end) noexcept {
    buffer = buffer.substr(0, ruvia::max_http_header_bytes);
    if (request_line_end != 0 || leading_bytes >= buffer.size()) {
        return http_parse_error::header_too_large;
    }
    const auto line = buffer.substr(leading_bytes);
    const auto method_end = line.find(' ');
    if (method_end == std::string_view::npos ||
        line.find_first_of(" \r\n", method_end + 1) != std::string_view::npos) {
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
[[nodiscard]] std::string_view bound_host_field_value(
    const request_target_view& target_view, std::string_view value) noexcept {
    return target_view.form_ == http_request_target_form::absolute ||
                   target_view.form_ == http_request_target_form::authority
               ? target_view.authority_
               : value;
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
            return !value.empty() && std::ranges::all_of(value, [](char ch) noexcept { return ch >= '0' && ch <= '9'; });
        };
        if (name_size != 0 && version.starts_with('/') && dot != std::string_view::npos &&
            digits(version.substr(1, dot - 1)) && digits(version.substr(dot + 1))) {
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

http1_server_request_parse_state::http1_server_request_parse_state() noexcept
    : request_(http_request_access::make()) {}

http_request http1_server_request_parse_state::take_request() && noexcept {
    return std::exchange(request_, http_request_access::make());
}

http1_request_body_plan http1_server_request_parse_state::take_body_plan() && noexcept {
    return std::exchange(body_plan_, http1_request_body_plan(http_request_expectations{}));
}

http1_request_connection_plan http1_server_request_parse_state::take_connection_plan() && noexcept {
    return std::exchange(connection_plan_, http1_request_connection_plan::http11_close());
}

void http1_server_request_parse_state::parse_head(
    std::string_view buffer, std::size_t header_search_offset) {
    // Bytes before header_search_offset were classified by an earlier call on
    // this state; an earlier search offset discards that progress.
    auto scan = need_request_head() != nullptr ? *need_request_head()
                                               : http1_server_need_request_head{};
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
            if (const auto error = classify_http_request_line_prefix(
                    buffer, scan.leading_bytes_, header_search_offset, scan.request_line_end_)) {
                return fail(*error);
            }
        }
        // A field section line ending in a bare LF can never complete the
        // CRLF-delimited head (RFC 9112 section 2.2); answer it at once rather
        // than waiting for the header timeout.
        if (scan.request_line_end_ != 0 &&
            has_http_bare_line_feed(buffer, std::max(scan.request_line_end_, header_search_offset))) {
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
    connection_plan_ = protocol_version == http_protocol_version::http11
                           ? plan_http11_request_connection(block.connection_options_.close())
                           : plan_http10_request_connection(block.connection_options_.close(),
                                 block.connection_options_.keep_alive());
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

    const auto* final_chunked =
        transfer_encoding.has_value() ? transfer_encoding->final_chunked() : nullptr;
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
        if (!http_request_access::add_header(
                request_, http_header_view{header_value.name_.bind(buffer), value}, header_value.kind_)) {
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
        body_plan_ =
            http1_request_body_plan(static_cast<std::size_t>(*content_length), expectations);
    } else {
        body_plan_ = http1_request_body_plan(expectations);
    }
    progress_ = http1_server_request_head_ready(header_bytes);
}

http_request make_parsed_http_request(
    borrowed_text method, borrowed_text target, std::span<const http_header_view> headers,
    borrowed_span<std::byte> body) {
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
        if (!is_valid_http_header_name(header.name()) ||
            !is_valid_http_header_value(header.value())) {
            throw http_parse_protocol_error(http_parse_error::invalid_header);
        }
        if (!budget.add_field(header.name(), header.value())) {
            throw http_parse_protocol_error(http_parse_error::header_too_large);
        }
        const auto kind = detail::classify_http_field(header.name());
        if (i < inline_kind_capacity) {
            inline_kinds[i] = kind;
        }
        if (detail::validate_request_field(kind, header.value(), detail::http_field_list_role::recipient, fields) !=
            detail::request_field_status::ok) {
            throw http_parse_protocol_error(kind == detail::http_field_kind::host
                                                ? http_parse_error::invalid_host
                                                : http_parse_error::invalid_header);
        }
    }

    apply_parsed_request_target(request, target_view);
    http_request_access::reserve_headers(request, headers.size());
    for (std::size_t i = 0; i < headers.size(); ++i) {
        auto field = headers[i];
        const auto kind = i < inline_kind_capacity
                              ? inline_kinds[i]
                              : detail::classify_http_field(field.name());
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

bool should_drop_invalid_cleartext_http1_input(
    std::string_view buffer, http1_server_request_parse_failure_source source_value) noexcept {
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
    return http_header_slice{
        .offset_ = static_cast<std::uint32_t>(offset), .length_ = static_cast<std::uint32_t>(length)};
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

    const auto parsed_version = parse_http1_version(version);
    if (parsed_version == http1_version_parse_result::invalid) {
        return http_parse_error::invalid_request_line;
    }
    if (parsed_version == http1_version_parse_result::unsupported) {
        return http_parse_error::unsupported_http_version;
    }
    block.protocol_version_ = parsed_version == http1_version_parse_result::http10
                                  ? http_protocol_version::http10
                                  : http_protocol_version::http11;
    return std::nullopt;
}

// What one request header means on HTTP/1: the shared semantic validator
// decides whether the value is acceptable, and the HTTP/1-specific fields are
// parsed into typed state on the block. Finding the bytes is the loop's job
// below; every rule about what they mean is here.
[[nodiscard]] std::optional<http_parse_error> apply_request_header(http_field_kind kind,
    std::string_view value, bool ignore_upgrade, parsed_request_header_block& block) {
    if (validate_request_field(kind, value, http_field_list_role::recipient, block.fields_) !=
        request_field_status::ok) {
        return kind == http_field_kind::host ? http_parse_error::invalid_host
                                             : http_parse_error::invalid_header;
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
            if (block.connection_options_.parse_field(value, http_connection_field_role::request_recipient) !=
                http_field_list_parse_status::ok) {
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
            if (!ignore_upgrade &&
                http_upgrade_protocols{}.parse_field(value, http_field_list_role::recipient) !=
                    http_field_list_parse_status::ok) {
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
[[nodiscard]] std::optional<http_parse_error> parse_header_fields(std::string_view buffer,
    std::size_t headers_end, std::size_t cursor_value, bool ignore_upgrade,
    parsed_request_header_block& block) {
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

        block.headers_.append(
            parsed_request_header_slot{.name_ = make_slice(static_cast<std::size_t>(name.data() - buffer.data()), name.size()),
                .value_ = make_slice(static_cast<std::size_t>(value.data() - buffer.data()), value.size()),
                .kind_ = kind});
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
        const auto i =
            buffer[cursor_value] == '\n' ? cursor_value : buffer.find('\n', cursor_value + 1);
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
    for (auto line_feed = buffer.find('\n', std::max<std::size_t>(search_offset, 1));
        line_feed != std::string_view::npos; line_feed = buffer.find('\n', line_feed + 1)) {
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

namespace ruvia {

http_protocol_error http_parse_protocol_error(http_parse_error error) noexcept {
    switch (error) {
        case http_parse_error::header_too_large:
            return http_protocol_error(http_status::request_header_fields_too_large,
                "header_too_large", "request header is too large");
        case http_parse_error::uri_too_long:
            return http_protocol_error(
                http_status::uri_too_long, "uri_too_long", "request target is too long");
        case http_parse_error::invalid_request_line:
            return http_protocol_error(
                http_status::bad_request, "invalid_request_line", "invalid request line");
        case http_parse_error::unsupported_http_version:
            return http_protocol_error(http_status::http_version_not_supported,
                "unsupported_http_version", "unsupported HTTP version");
        case http_parse_error::invalid_request_target:
            return http_protocol_error(
                http_status::bad_request, "invalid_request_target", "invalid request target");
        case http_parse_error::invalid_header:
            return http_protocol_error(
                http_status::bad_request, "invalid_header", "invalid request header");
        case http_parse_error::invalid_connection:
            return http_protocol_error(
                http_status::bad_request, "invalid_connection", "invalid Connection header");
        case http_parse_error::invalid_upgrade:
            return http_protocol_error(
                http_status::bad_request, "invalid_upgrade", "invalid Upgrade header");
        case http_parse_error::missing_host:
            return http_protocol_error(
                http_status::bad_request, "missing_host", "missing Host header");
        case http_parse_error::invalid_host:
            return http_protocol_error(
                http_status::bad_request, "invalid_host", "invalid Host header");
        case http_parse_error::invalid_content_length:
            return http_protocol_error(http_status::bad_request, "invalid_content_length",
                "invalid Content-Length header");
        case http_parse_error::conflicting_content_length:
            return http_protocol_error(http_status::bad_request, "conflicting_content_length",
                "conflicting Content-Length header");
        case http_parse_error::invalid_transfer_encoding:
            return http_protocol_error(http_status::bad_request, "invalid_transfer_encoding",
                "invalid Transfer-Encoding header");
        case http_parse_error::unsupported_transfer_encoding:
            return http_protocol_error(http_status::not_implemented,
                "unsupported_transfer_encoding", "unsupported transfer encoding");
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
[[nodiscard]] bool is_valid_http1_emitted_response_header(std::string_view name, std::string_view value,
    bool trailer_header_allowed, http_connection_options& connection_options,
    http_upgrade_protocols& upgrade_protocols) noexcept {
    const auto kind = classify_http_field(name);
    if (kind == http_field_kind::trailer) {
        return trailer_header_allowed || http_trim_ows(value).empty();
    }
    return parse_http_response_control_field(kind, value, connection_options, upgrade_protocols) ==
           http_response_control_field_status::ok;
}

void emit_response_head(const http_response& response, http_raw_wire_sink& sink_value, http_status_code response_status,
    std::string_view reason_phrase, std::string_view date_header, bool emit_chunked_transfer_encoding,
    std::string_view content_length_value) noexcept {
    emit_http1_status_line(sink_value, response_status, reason_phrase);

    for (const auto& header : response.headers()) {
        const auto known_bit = http_response_header_access::known_bit(header);
        // HTTP/1 framing belongs exclusively to http1_response_head_plan: the
        // handler's Content-Length and Transfer-Encoding (only ever the
        // replaceable chunked hint) yield to the plan's own framing fields.
        if (known_bit == response_header_transfer_encoding ||
            known_bit == response_header_content_length) {
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
void append_response_trailer_snapshot(std::string& output,
    std::span<const http_header_view> fields, std::size_t bytes) {
    std::array<char, max_http_header_bytes> scratch;
    http_raw_wire_sink sink(scratch.data());
    emit_response_trailer_fields(sink, fields);
    check_http_wire_end(sink.position(), scratch.data() + bytes);
    output.append(scratch.data(), bytes);
}

}  // namespace

}  // namespace ruvia::detail

namespace ruvia {

void append_http1_response_head(
    const http_response& response, http_response_head_buffer& head, const http1_response_head_plan& plan) {
    const auto body_plan = plan.body_plan();
    const auto* buffered = plan.buffered();
    const auto content_length_plan = buffered != nullptr
                                         ? detail::http_framed_buffered_response_head_plan(body_plan, buffered->content_length(), response)
                                         : detail::http_framed_streaming_response_head_plan(body_plan, response);
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
    const bool chunked_payload_plan = plan.chunked_stream() != nullptr &&
                                      body_plan.transfer_encoding_allowed() && !body_plan.body_suppressed();
    if (chunked_payload_plan && plan.protocol_version() == http_protocol_version::http10) {
        throw http1_response_head_error("HTTP/1.0 response cannot use chunked Transfer-Encoding");
    }
    const bool emit_chunked_transfer_encoding = chunked_payload_plan;
    // HTTP/1 alone can frame a stream with chunked coding or connection close.
    // Those delimiters replace Content-Length except suppressed HEAD metadata.
    const auto content_length = emit_chunked_transfer_encoding ||
                                        (plan.close_delimited_stream() != nullptr && !body_plan.body_suppressed())
                                    ? std::nullopt
                                    : framed->content_length();
    const auto known_bits = detail::response_known_header_bits(response);

    const auto reason_phrase = http_reason_phrase(response_status);
    const auto date_header = (known_bits & detail::response_header_date) == 0
                                 ? detail::cached_date_header()
                                 : std::string_view{};
    // Reuse the formatted token for exact measurement and emission instead of
    // separately dividing the length to count its digits.
    // Buffered HEAD metadata retains the selected representation length.
    // A status-level no-content policy that still owns framing (205) is
    // canonicalized to zero for both buffered and streaming heads.
    std::array<char, http_max_unsigned_decimal_size> content_length_buffer;
    const auto content_length_value =
        content_length.has_value()
            ? http_format_unsigned(content_length_buffer, *content_length)
            : std::string_view{};

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
        if (known_bit == detail::response_header_transfer_encoding ||
            known_bit == detail::response_header_content_length) {
            continue;
        }
        if (!detail::is_valid_http1_emitted_response_header(header.name(), header.value(),
                emit_chunked_transfer_encoding, connection_options, upgrade_protocols)) {
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
        detail::emit_response_head(response, sink_value, response_status, reason_phrase, date_header,
            emit_chunked_transfer_encoding, content_length_value);
        detail::check_http_wire_end(sink_value.position(), cursor_value + head_budget.bytes());
    });
}

void append_http1_response_trailers(
    std::string& output, const http_response_trailer_section& trailers) {
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
        response.header(detail::http_connection_field_name, option,
            http_response_header_options{.mode_ = http_response_header_mode::append});
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
        (void)detail::http_upgrade_protocols{}.parse_field(headers[index].value(), detail::http_field_list_role::recipient,
            [&visiting, &visitor](const detail::http_upgrade_protocol& protocol) noexcept {
                visiting = visitor(protocol);
                return visiting;
            });
    }
}

[[nodiscard]] bool request_offers_upgrade_protocol(
    const http_request& request, const detail::http_upgrade_protocol& selected) noexcept {
    bool offered = false;
    visit_offered_upgrade_protocols(request, [&offered, &selected](const detail::http_upgrade_protocol& protocol) noexcept {
        offered = detail::http_upgrade_protocol_equals(protocol, selected);
        return !offered;
    });
    return offered;
}

}  // namespace

http1_request_connection_plan http1_commit_final_response(
    http_response& response, http1_request_connection_plan plan) {
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
        http1_mark_connection_close(response, preserve_upgrade
                                                  ? http1_connection_close_field_policy::preserve_upgrade
                                                  : http1_connection_close_field_policy::close_only);
    } else {
        const bool generate_upgrade_option = preserve_upgrade && !response_options.upgrade();
        if (generate_upgrade_option) {
            add_http1_connection_option(
                response, detail::http_connection_upgrade_option, response_options.has_field());
        }
        if (plan.protocol_version() == http_protocol_version::http10 && !response_options.keep_alive()) {
            add_http1_connection_option(response, detail::http_connection_keep_alive_option,
                response_options.has_field() || generate_upgrade_option);
        }
    }
    return plan;
}

prepared_http1_response_stream prepare_http1_response_stream_head(
    http_response response, const http1_response_stream_plan& plan,
    http_response_trailer_intent trailer_intent) {
    auto commit_plan = plan_http_response_stream_commit(
        plan.framing(), plan.request_method(), response.status(), trailer_intent);
    const auto body_plan = commit_plan.body_plan();
    // HTTP/1.0 cannot delimit an open-ended response stream without closing the
    // connection, but a response whose method/status forbids payload is already
    // self-delimited. Make this decision at head commit, when the response status is
    // finally known, instead of pessimistically baking close into the pre-commit plan.
    const auto planned_connection =
        plan.request_connection_plan().disposition() == http1_close_policy::allow_reuse &&
                plan.close_policy() == http1_close_policy::allow_reuse &&
                (plan.framing() != http_response_stream_framing::http1_close_delimited ||
                    body_plan.body_suppressed())
            ? plan.request_connection_plan()
            : plan.request_connection_plan().require_close();
    const auto connection_plan = http1_commit_final_response(response, planned_connection);
    auto head = prepare_http_response_stream_head(std::move(response), std::move(commit_plan));
    const auto response_head_plan =
        plan.framing() == http_response_stream_framing::http1_chunked
            ? http1_chunked_response_stream_head_plan(head.commit_plan().body_plan(), connection_plan)
            : http1_close_delimited_response_stream_head_plan(
                  head.commit_plan().body_plan(), connection_plan);
    return prepared_http1_response_stream(std::move(head), response_head_plan, connection_plan);
}

bool http1_request_offers_upgrade(const http_request& request) noexcept {
    if (request.protocol_version() != http_protocol_version::http11 ||
        request.known_method() == http_known_method::connect) {
        return false;
    }
    detail::http_connection_options connection_options;
    detail::http_upgrade_protocols upgrade_protocols;
    const auto headers = request.headers();
    for (std::size_t index = 0; index < headers.size(); ++index) {
        const auto kind = detail::http_request_access::header_kind(request, index);
        if (kind == detail::http_field_kind::connection) {
            if (connection_options.parse_field(headers[index].value(),
                    detail::http_connection_field_role::request_recipient) != detail::http_field_list_parse_status::ok) {
                return false;
            }
        } else if (kind == detail::http_field_kind::upgrade) {
            if (upgrade_protocols.parse_field(headers[index].value(), detail::http_field_list_role::recipient) !=
                detail::http_field_list_parse_status::ok) {
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
    if (response.file_body() || response.has_multipart_file_body() || !response.body_bytes().empty() ||
        detail::response_has_known_header(
            response, detail::response_header_content_length | detail::response_header_transfer_encoding)) {
        throw http1_response_head_error("HTTP/1 Switching Protocols response cannot carry content or framing fields");
    }
    const auto http1_control = detail::http1_final_response_control_plan(response);
    // RFC 9110 §7.8: the server switches only to protocols the client offered.
    if (http1_control.upgrade_protocols().has_field()) {
        bool offered = http1_control.upgrade_protocols().has_protocol();
        for (const auto& header : response.headers()) {
            if (offered && detail::http_response_header_access::known_bit(header) == detail::response_header_upgrade) {
                (void)detail::http_upgrade_protocols{}.parse_field(header.value(), detail::http_field_list_role::sender,
                    [&offered, &request](const detail::http_upgrade_protocol& selected) noexcept {
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
            const auto end = protocol.version_.empty() ? protocol.name_.data() + protocol.name_.size()
                                                       : protocol.version_.data() + protocol.version_.size();
            offered_protocol = std::string_view(protocol.name_.data(), static_cast<std::size_t>(end - protocol.name_.data()));
            return false;
        });
        response.header("Upgrade", offered_protocol);
    }
    if (!http1_control.connection_options().upgrade()) {
        add_http1_connection_option(response, detail::http_connection_upgrade_option,
            http1_control.connection_options().has_field());
    }
    response.status_code_ = http_status::switching_protocols;
    return http1_close_delimited_response_stream_head_plan(
        plan_http_response_body(http_known_method::get, http_status::switching_protocols),
        http1_request_connection_plan::http11_close());
}

}  // namespace ruvia

namespace ruvia {

http_response_head_buffer::http_response_head_buffer(const http_response_head_buffer& other)
    : heap_(other.heap_) {
}

http_response_head_buffer::http_response_head_buffer(http_response_head_buffer&& other) noexcept
    : heap_(std::move(other.heap_)) {
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

http_response_head_buffer& http_response_head_buffer::operator=(
    http_response_head_buffer&& other) noexcept {
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
