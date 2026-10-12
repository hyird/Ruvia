#include "ruvia/http/http1_server_request_parser.h"

#include <algorithm>

#include "ruvia/http/http_field_section.h"
#include "ruvia/http/http_field_syntax.h"
#include "ruvia/http/http_limits.h"
#include "ruvia/http/http_request_content_semantics.h"

#include "parser/http_header_block_parser.h"
#include "parser/http_request_target.h"
#include "request/http_request_access.h"

namespace ruvia {
namespace {

using ruvia::detail::classify_http_request_line_prefix;
using ruvia::detail::find_http_request_head_end;
using ruvia::detail::http_request_access;
using ruvia::detail::parse_http_header_block;
using ruvia::detail::parse_request_target;
using ruvia::detail::parsed_request_header_block;
using ruvia::detail::request_header_kind;
using ruvia::detail::request_target_view;

}  // namespace

http1_server_request_parse_state::http1_server_request_parse_state() noexcept
    : request_(http_request_access::make()) {}

void http1_server_request_parser::parse_head(std::string_view buffer,
    http1_server_request_parse_state& state_value, std::size_t header_search_offset,
    std::pmr::memory_resource* resource) const {
    // Bytes before header_search_offset were classified by an earlier call on
    // this state; an earlier search offset discards that progress.
    auto scan = state_value.need_request_head() != nullptr ? *state_value.need_request_head()
                                                           : http1_server_need_request_head{};
    scan.leading_bytes_ = std::min(scan.leading_bytes_, header_search_offset);
    scan.request_line_end_ = scan.request_line_end_ <= header_search_offset ? scan.request_line_end_ : 0;

    // Incomplete input allocates no descriptor storage.
    state_value.body_plan_ = http1_request_body_plan(http_request_expectations{});
    state_value.connection_plan_ = http1_request_connection_plan::http11_close();
    state_value.response_coding_qualities_ = {};
    http_request_access::reset(state_value.request_);

    const auto fail = [&state_value](http_parse_error error) noexcept {
        // A rejected head leaves the message boundary unknown and its content
        // unconsumed, so the connection cannot be reused (RFC 9112 sections 6.1
        // and 6.3). The request version may already have been accepted when a
        // later target/framing semantic check fails; preserve it so an HTTP/1.0
        // error response is not silently upgraded to HTTP/1.1.
        const auto connection_plan = state_value.connection_plan_.require_close();
        http_request_access::reset(state_value.request_);
        state_value.progress_ = http1_server_request_parse_failure(error);
        state_value.connection_plan_ = connection_plan;
    };

    const auto header_bytes = find_http_request_head_end(buffer, scan.leading_bytes_, header_search_offset);
    if (header_bytes == std::string_view::npos) {
        if (scan.request_line_end_ == 0) {
            if (const auto error = classify_http_request_line_prefix(
                    buffer, scan.leading_bytes_, header_search_offset, scan.request_line_end_)) {
                return fail(*error);
            }
        }
        if (buffer.size() >= max_http_header_bytes) {
            return fail(http_parse_error::header_too_large);
        }
        state_value.progress_ = scan;
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
    http_request_access::set_method(state_value.request_, method);
    const auto known_method = state_value.request_.known_method();

    const auto target = block.target_.bind(buffer);
    http_request_access::set_target(state_value.request_, target);

    const auto protocol_version = block.protocol_version_;
    http_request_access::set_protocol_version(state_value.request_, protocol_version);
    // Publish the version-specific request contract before any validation that
    // can fail after the version line itself has been accepted. The final
    // disposition is already derived from the parsed Connection fields and is
    // tightened to close by body/response policy later.
    state_value.connection_plan_ = protocol_version == http_protocol_version::http11
                                       ? plan_http11_request_connection(block.connection_options_.close())
                                       : plan_http10_request_connection(block.connection_options_.close(),
                                             block.connection_options_.keep_alive());
    if (known_method == http_known_method::connect) {
        state_value.connection_plan_ = state_value.connection_plan_.with_tunnel_request();
    }
    if (block.upgrade_protocols_.has_field() && !block.connection_options_.upgrade()) {
        return fail(http_parse_error::invalid_connection);
    }
    // RFC 9110 10.1.4 obliges the TE sender to list "TE" in Connection; it does
    // not oblige the recipient to reject. The server never acts on TE (it does
    // not condition trailers on it), so a bare TE is simply ignored.

    request_target_view target_view;
    if (!parse_request_target(known_method, target, target_view)) {
        return fail(http_parse_error::invalid_request_target);
    }
    http_request_access::set_path(state_value.request_, target_view.path_);
    http_request_access::set_query_string(state_value.request_, target_view.query_);
    http_request_access::set_scheme(state_value.request_, target_view.scheme_);
    http_request_access::set_authority(state_value.request_, target_view.authority_);
    http_request_access::set_target_form(state_value.request_, target_view.form_);

    if (protocol_version == http_protocol_version::http11 && block.host_header_index_ < 0) {
        return fail(http_parse_error::missing_host);
    }
    const auto host_header_index = block.host_header_index_;

    const auto content_length = block.content_length_.value();
    const auto& transfer_encoding = block.transfer_encoding_.value();
    if (transfer_encoding.has_value() && content_length.has_value()) {
        return fail(http_parse_error::invalid_transfer_encoding);
    }

    if (http_request_content_semantics(method) == http_request_content_semantics::forbidden) {
        // CONNECT has no request content, and TRACE explicitly forbids it
        // (RFC 9110 sections 9.3.6 and 9.3.8). Content-Length is an explicit
        // content signal even at zero; accepting either framing field would
        // give the runtime a body contract that the method does not have.
        if (transfer_encoding.has_value()) {
            return fail(http_parse_error::invalid_transfer_encoding);
        }
        if (content_length.has_value()) {
            return fail(http_parse_error::invalid_content_length);
        }
    }

    const auto* final_chunked =
        transfer_encoding.has_value() ? transfer_encoding->final_chunked() : nullptr;
    if (transfer_encoding.has_value() && final_chunked == nullptr) {
        return fail(http_parse_error::invalid_transfer_encoding);
    }
    if (block.non_empty_trailer_header_present_ && final_chunked == nullptr) {
        return fail(http_parse_error::invalid_header);
    }

    // RFC 9112 section 6.1: Transfer-Encoding in an HTTP/1.0 request must be treated
    // as faulty framing; the error path closes the connection after replying.
    if (transfer_encoding.has_value() && protocol_version == http_protocol_version::http10) {
        return fail(http_parse_error::invalid_transfer_encoding);
    }
    if (block.transfer_encoding_.unsupported()) {
        return fail(http_parse_error::unsupported_transfer_encoding);
    }

    if (http_request_content_semantics(method) == http_request_content_semantics::content_type_required &&
        (content_length.has_value() || transfer_encoding.has_value()) &&
        !block.fields_.seen(request_header_kind::content_type)) {
        // RFC 9110 section 9.3.7 requires a valid Content-Type when OPTIONS
        // explicitly carries content. A zero Content-Length still declares an
        // empty representation and therefore retains this metadata contract.
        return fail(http_parse_error::invalid_header);
    }

    http_request_access::set_resource(state_value.request_, resource);
    http_request_access::reserve_headers(state_value.request_, block.header_count_);
    for (std::size_t i = 0; i < block.header_count_; ++i) {
        const auto& header_value = block.headers_[i];
        auto value = header_value.value_.bind(buffer);
        // RFC 9112 sections 3.2.2 and 3.3 make the request-target authoritative
        // for absolute-form and authority-form. Rebind both headers() and the
        // known-header cache so application code cannot observe a conflicting
        // Host value as a second routing truth.
        if ((target_view.form_ == http_request_target_form::absolute ||
                target_view.form_ == http_request_target_form::authority) &&
            host_header_index >= 0 && i == static_cast<std::size_t>(host_header_index)) {
            value = target_view.authority_;
        }
        (void)http_request_access::add_header(state_value.request_,
            http_header_view{header_value.name_.bind(buffer), value},
            request_header_kind_known_slot(header_value.kind_));
    }

    state_value.response_coding_qualities_ = block.response_coding_qualities_;
    auto expectations = block.expectations_;
    if (protocol_version == http_protocol_version::http10) {
        expectations.ignore_continue();
    }
    if (final_chunked != nullptr) {
        state_value.body_plan_ = http1_request_body_plan(final_chunked->transfer_codings(), expectations);
    } else if (content_length.has_value()) {
        state_value.body_plan_ = http1_request_body_plan(*content_length, expectations);
    } else {
        state_value.body_plan_ = http1_request_body_plan(expectations);
    }
    state_value.progress_ = http1_server_request_head_ready(header_bytes);
}

std::pair<http_request, std::optional<http_parse_error>> make_parsed_http_request(
    std::string_view method, std::string_view target, std::span<const http_header_view> headers,
    std::span<const std::byte> body, std::pmr::memory_resource* resource) {
    auto request = http_request_access::make();
    http_request_access::set_resource(request, resource);
    http_request_access::set_method(request, method);
    http_request_access::set_target(request, target);

    std::optional<http_parse_error> error;
    request_target_view target_view;
    if (!is_valid_http_method_token(method)) {
        error = http_parse_error::invalid_request_line;
    } else if (!parse_request_target(request.known_method(), target, target_view)) {
        error = http_parse_error::invalid_request_target;
    } else if (headers.size() > max_http_header_fields) {
        error = http_parse_error::too_many_headers;
    } else {
        http_field_section_budget budget;
        for (const auto& header : headers) {
            if (!is_valid_http_header_name(header.name()) ||
                !is_valid_http_header_value(header.value())) {
                error = http_parse_error::invalid_header;
                break;
            }
            if (!budget.add_field(header.name(), header.value())) {
                error = http_parse_error::header_too_large;
                break;
            }
        }
    }
    if (error.has_value()) {
        return {std::move(request), error};
    }

    http_request_access::set_path(request, target_view.path_);
    http_request_access::set_query_string(request, target_view.query_);
    http_request_access::set_scheme(request, target_view.scheme_);
    http_request_access::set_authority(request, target_view.authority_);
    http_request_access::set_target_form(request, target_view.form_);
    http_request_access::reserve_headers(request, headers.size());
    for (const auto& header : headers) {
        if (!http_request_access::add_header(request, header)) {
            error = http_parse_error::too_many_headers;
            break;
        }
    }
    http_request_access::set_body(request, body);
    return {std::move(request), error};
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
    if (!is_http_token_char(static_cast<unsigned char>(buffer.front()))) {
        return true;
    }

    const auto line_end = buffer.find("\r\n");
    if (line_end == std::string_view::npos) {
        return false;
    }

    auto line = buffer.substr(0, line_end);
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) {
        line.remove_suffix(1);
    }

    const auto version_start = line.find_last_of(" \t");
    if (version_start == std::string_view::npos || version_start + 1 >= line.size()) {
        return false;
    }
    return !line.substr(version_start + 1).starts_with("HTTP/");
}

}  // namespace ruvia
