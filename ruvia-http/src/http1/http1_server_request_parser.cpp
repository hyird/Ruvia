#include "ruvia/http/http1_server_request_parser.h"

#include "ruvia/http/http1_request_parser.h"
#include "ruvia/http/http_limits.h"
#include "ruvia/http/http_request_content_semantics.h"

#include "parser/http_chunk_parser.h"
#include "parser/http_header_block_parser.h"
#include "parser/http_request_target.h"
#include "request/http_request_access.h"

namespace ruvia {
namespace {

using ruvia::detail::find_http_header_end;
using ruvia::detail::http_chunk_scan_error;
using ruvia::detail::http_request_access;
using ruvia::detail::parse_http_header_block;
using ruvia::detail::parse_request_target;
using ruvia::detail::parsed_request_header_block;
using ruvia::detail::request_header_kind;
using ruvia::detail::request_target_view;
using ruvia::detail::scan_http_chunked_body;
using ruvia::detail::singleton_request_header_bit;

}  // namespace

http1_server_request_parse_state::http1_server_request_parse_state() noexcept
    : request_(http_request_access::make()) {}

void http1_server_request_parser::parse_request_head(std::string_view buffer,
    std::size_t header_search_offset, http1_server_request_parse_state& state_value,
    std::pmr::memory_resource* resource) {
    // Incomplete input allocates no descriptor storage.
    state_value.progress_ = http1_server_need_request_head{};
    state_value.body_plan_ = http1_request_body_plan(http_request_expectations{});
    state_value.connection_plan_ = http1_request_connection_plan::http11_close();
    state_value.response_coding_qualities_ = {};
    http_request_access::reset(state_value.request_);

    const auto fail = [&state_value](http_parse_error error) noexcept {
        // The request version may already have been accepted when a later
        // target/framing semantic check fails. Preserve that protocol contract
        // so an HTTP/1.0 error response is not silently upgraded to HTTP/1.1.
        const auto connection_plan = state_value.connection_plan_;
        http_request_access::reset(state_value.request_);
        state_value.progress_ = http1_server_request_parse_failure(error);
        state_value.connection_plan_ = connection_plan;
    };

    const auto header_bytes = find_http_header_end(buffer, header_search_offset);
    if (header_bytes == std::string_view::npos) {
        if (buffer.size() >= max_http_header_bytes) {
            return fail(http_parse_error::header_too_large);
        }
        return;
    }

    if (header_bytes > max_http_header_bytes) {
        return fail(http_parse_error::header_too_large);
    }

    parsed_request_header_block block(resource);
    if (const auto error = parse_http_header_block(buffer, header_bytes, block)) {
        return fail(*error);
    }

    // parse_http_header_block scans the method through the token table. Preserve the
    // exact wire token: method registration is extensible, while http_known_method is
    // only the framework's routing/response-semantics classification.
    const auto method = block.method_.bind(buffer);
    http_request_access::set_method(state_value.request_, method);
    const auto known_method = state_value.request_.known_method();

    const auto target = block.target_.bind(buffer);
    const auto version = block.version_.bind(buffer);
    http_request_access::set_target(state_value.request_, target);

    if (version.size() != 8 || !version.starts_with("HTTP/") || version[5] < '0' ||
        version[5] > '9' || version[6] != '.') {
        return fail(http_parse_error::invalid_request_line);
    }
    if (version[7] < '0' || version[7] > '9') {
        return fail(http_parse_error::invalid_request_line);
    }
    if (version[5] != '1' || (version[7] != '0' && version[7] != '1')) {
        return fail(http_parse_error::unsupported_http_version);
    }
    const auto protocol_version =
        version[7] == '1' ? http_protocol_version::http11 : http_protocol_version::http10;
    http_request_access::set_protocol_version(state_value.request_, protocol_version);
    // Publish the version-specific request contract before any validation that
    // can fail after the version line itself has been accepted. The final
    // disposition is already derived from the parsed Connection fields and is
    // tightened to close by body/response policy later.
    state_value.connection_plan_ = protocol_version == http_protocol_version::http11
                                       ? plan_http11_request_connection(block.connection_options_.close())
                                       : plan_http10_request_connection(block.connection_options_.close(),
                                             block.connection_options_.keep_alive());
    if (block.upgrade_protocols_.has_field() && !block.connection_options_.upgrade()) {
        return fail(http_parse_error::invalid_connection);
    }
    if (block.te_header_present_ && !block.connection_options_.te()) {
        return fail(http_parse_error::invalid_connection);
    }

    request_target_view target_view;
    if (!parse_request_target(known_method, target, target_view)) {
        return fail(http_parse_error::invalid_request_target);
    }
    http_request_access::set_path(state_value.request_, target_view.path_);
    http_request_access::set_query_string(state_value.request_, target_view.query_);
    http_request_access::set_scheme(state_value.request_, target_view.scheme_);
    http_request_access::set_authority(state_value.request_, target_view.authority_);
    switch (target_view.form_) {
        case detail::http_request_target_form::origin:
            http_request_access::set_target_form(
                state_value.request_, ::ruvia::http_request_target_form::origin);
            break;
        case detail::http_request_target_form::absolute:
            http_request_access::set_target_form(
                state_value.request_, ::ruvia::http_request_target_form::absolute);
            break;
        case detail::http_request_target_form::authority:
            http_request_access::set_target_form(
                state_value.request_, ::ruvia::http_request_target_form::authority);
            break;
        case detail::http_request_target_form::asterisk:
            http_request_access::set_target_form(
                state_value.request_, ::ruvia::http_request_target_form::asterisk);
            break;
    }

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
        state_value.connection_plan_ = state_value.connection_plan_.require_close();
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
        state_value.connection_plan_ = state_value.connection_plan_.require_close();
        return fail(http_parse_error::unsupported_transfer_encoding);
    }

    if (http_request_content_semantics(method) == http_request_content_semantics::content_type_required &&
        (content_length.has_value() || transfer_encoding.has_value()) &&
        (block.seen_header_bits_ & singleton_request_header_bit(request_header_kind::content_type)) == 0) {
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
        if ((target_view.form_ == detail::http_request_target_form::absolute ||
                target_view.form_ == detail::http_request_target_form::authority) &&
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

void http1_server_request_parser::parse_head(std::string_view buffer,
    http1_server_request_parse_state& state_value, std::size_t header_search_offset,
    std::pmr::memory_resource* resource) const {
    parse_request_head(buffer, header_search_offset, state_value, resource);
}

void http1_server_request_parser::parse_message_body(
    std::string_view buffer, http1_server_request_parse_state& state_value) noexcept {
    const auto* request_head = state_value.head_ready();
    if (request_head == nullptr) {
        return;
    }

    const auto header_bytes = request_head->header_bytes();

    const auto fail = [&state_value](http_parse_error error) noexcept {
        const auto connection_plan = state_value.connection_plan_.require_close();
        http_request_access::reset(state_value.request_);
        state_value.progress_ = http1_server_request_parse_failure(error);
        state_value.body_plan_ = http1_request_body_plan(http_request_expectations{});
        state_value.connection_plan_ = connection_plan;
    };
    // header_bytes is captured rather than passed: every call site forwards the
    // same head length, and a parameter of that name would shadow it.
    const auto need_more = [&state_value, header_bytes]() noexcept {
        // The request views borrow `buffer`. A caller must reparse after growing
        // or moving that buffer, so an incomplete message intentionally exposes
        // no apparently reusable request head.
        http_request_access::reset(state_value.request_);
        state_value.progress_ = http1_server_need_request_body(header_bytes);
    };
    const auto need_more_until = [&state_value, header_bytes](std::size_t required_total_bytes) noexcept {
        // The request views borrow `buffer`. A caller must reparse after growing
        // or moving that buffer, so an incomplete message intentionally exposes
        // no apparently reusable request head.
        http_request_access::reset(state_value.request_);
        state_value.progress_ = http1_server_need_request_body(header_bytes, required_total_bytes);
    };

    const auto& body_plan = state_value.body_plan_;
    const auto* chunked_body = body_plan.chunked();
    const auto* known_length_body = body_plan.known_length();
    std::size_t message_bytes = 0;
    if (chunked_body != nullptr) {
        const auto chunked = scan_http_chunked_body(buffer.substr(header_bytes));
        if (const auto* complete = chunked.complete()) {
            message_bytes = header_bytes + complete->consumed_bytes();
        } else if (chunked.need_more() != nullptr) {
            return need_more();
        } else {
            switch (chunked.failure()->error()) {
                case http_chunk_scan_error::invalid_size:
                    return fail(http_parse_error::invalid_chunk_size);
                case http_chunk_scan_error::size_overflow:
                    return fail(http_parse_error::chunk_size_overflow);
                case http_chunk_scan_error::invalid_extension:
                    return fail(http_parse_error::invalid_chunk_extension);
                case http_chunk_scan_error::invalid_crlf:
                    return fail(http_parse_error::invalid_chunk_crlf);
                case http_chunk_scan_error::invalid_trailer:
                    return fail(http_parse_error::invalid_trailer);
                case http_chunk_scan_error::too_large:
                    return fail(http_parse_error::body_too_large);
            }
        }
    } else if (known_length_body != nullptr) {
        const auto content_length = known_length_body->content_length();
        if (content_length > default_max_buffered_body_bytes ||
            content_length > max_http_request_bytes - header_bytes) {
            return fail(http_parse_error::body_too_large);
        }
        message_bytes = header_bytes + content_length;
    } else {
        message_bytes = header_bytes;
    }
    if (message_bytes > max_http_request_bytes) {
        return fail(http_parse_error::body_too_large);
    }
    if (buffer.size() < message_bytes) {
        return need_more_until(message_bytes);
    }

    http_request_access::set_body(state_value.request_,
        known_length_body != nullptr ? buffer.substr(header_bytes, known_length_body->content_length())
                                     : std::string_view{});
    state_value.progress_ = http1_server_request_message_ready(header_bytes, message_bytes);
}

http1_server_request_parse_state http1_server_request_parser::parse_message(
    std::string_view buffer, std::pmr::memory_resource* resource) const {
    http1_server_request_parse_state state;
    parse_request_head(buffer, 0, state, resource);
    parse_message_body(buffer, state);
    return state;
}

}  // namespace ruvia

namespace ruvia {

std::pair<http_request, std::optional<http_parse_error>> make_parsed_http_request(
    std::string_view method, std::string_view target, std::span<const http_header_view> headers,
    std::span<const std::byte> body, std::pmr::memory_resource* resource) {
    auto request = detail::http_request_access::make();
    detail::http_request_access::set_resource(request, resource);
    detail::http_request_access::set_method(request, method);
    detail::http_request_access::set_target(request, target);

    std::optional<http_parse_error> error;
    detail::request_target_view target_view;
    if (!is_valid_http_method_token(method)) {
        error = http_parse_error::invalid_request_line;
    } else if (!detail::parse_request_target(request.known_method(), target, target_view)) {
        error = http_parse_error::invalid_request_target;
    } else if (headers.size() > max_http_header_fields) {
        error = http_parse_error::too_many_headers;
    } else {
        std::size_t header_bytes = 0;
        for (const auto& header : headers) {
            if (!detail::is_valid_http_header_name(header.name()) ||
                !detail::is_valid_http_header_value(header.value())) {
                error = http_parse_error::invalid_header;
                break;
            }
            const auto remaining = max_http_header_bytes - header_bytes;
            if (header.name().size() > remaining ||
                header.value().size() > remaining - header.name().size() ||
                remaining - header.name().size() - header.value().size() < 4) {
                error = http_parse_error::header_too_large;
                break;
            }
            header_bytes += header.name().size() + header.value().size() + 4;
        }
    }
    if (error.has_value()) {
        return {std::move(request), error};
    }

    detail::http_request_access::set_path(request, target_view.path_);
    detail::http_request_access::set_query_string(request, target_view.query_);
    detail::http_request_access::set_scheme(request, target_view.scheme_);
    detail::http_request_access::set_authority(request, target_view.authority_);
    switch (target_view.form_) {
        case detail::http_request_target_form::origin:
            detail::http_request_access::set_target_form(request, ::ruvia::http_request_target_form::origin);
            break;
        case detail::http_request_target_form::absolute:
            detail::http_request_access::set_target_form(request, ::ruvia::http_request_target_form::absolute);
            break;
        case detail::http_request_target_form::authority:
            detail::http_request_access::set_target_form(request, ::ruvia::http_request_target_form::authority);
            break;
        case detail::http_request_target_form::asterisk:
            detail::http_request_access::set_target_form(request, ::ruvia::http_request_target_form::asterisk);
            break;
    }
    detail::http_request_access::reserve_headers(request, headers.size());
    for (const auto& header : headers) {
        if (!detail::http_request_access::add_header(request, header)) {
            error = http_parse_error::too_many_headers;
            break;
        }
    }
    detail::http_request_access::set_body(request, body);
    return {std::move(request), error};
}

}  // namespace ruvia

namespace ruvia {

bool should_drop_invalid_cleartext_http1_input(
    std::string_view buffer, http1_request_parse_failure_source source_value) noexcept {
    if (source_value != http1_request_parse_failure_source::request_line) {
        return false;
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

http1_request_parse_result http1_request_parser::parse(std::string_view buffer,
    http1_request_parse_options options) const {
    http1_server_request_parser parser;
    auto parsed_value = parser.parse_message(buffer, options.resource_);
    if (parsed_value.need_request_head() != nullptr) {
        return detail::http1_request_parse_result_access::need_more();
    }
    if (const auto* need_body = parsed_value.need_request_body()) {
        if (const auto required_total_bytes = need_body->required_total_bytes()) {
            return detail::http1_request_parse_result_access::need_more(*required_total_bytes);
        }
        return detail::http1_request_parse_result_access::need_more();
    }
    if (const auto* failure = parsed_value.failure()) {
        return detail::http1_request_parse_result_access::failure(*failure);
    }
    const auto* message = parsed_value.message_ready();
    if (message == nullptr) {
        std::terminate();
    }

    const auto wire_body =
        buffer.substr(message->header_bytes(), message->message_bytes() - message->header_bytes());
    return detail::http1_request_parse_result_access::parsed(
        std::move(parsed_value.request_), std::move(parsed_value.body_plan_), wire_body, message->message_bytes());
}

}  // namespace ruvia
