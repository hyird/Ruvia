#include "ruvia/http/http1_client_request_writer.h"

#include <array>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <system_error>

#include "ruvia/http/detail/field/http_expectations.h"
#include "ruvia/http/detail/parser/http_parser_syntax.h"
#include "ruvia/http/detail/util/pmr_resource.h"
#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_connect_udp.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_known_method.h"
#include "ruvia/http/http_request_content_semantics.h"

#include "client/http1_client_request_headers.h"
#include "client/http_origin_view.h"
#include "parser/http_request_target.h"

namespace ruvia::detail {

struct http1_client_request_prepare_result_access final {
    [[nodiscard]] static http1_client_exchange_state exchange_state(http_known_method method,
        http_connection_options connection_options, http1_close_policy close_policy,
        http1_client_initial_content_state content_state,
        std::pmr::string offered_upgrade_protocols) noexcept {
        return http1_client_exchange_state(method, connection_options, close_policy, content_state,
            std::move(offered_upgrade_protocols));
    }

    [[nodiscard]] static constexpr http1_client_request_prepare_result buffer_too_small(
        std::size_t required_head_bytes) noexcept {
        return http1_client_request_prepare_result(http1_client_request_buffer_too_small(required_head_bytes));
    }

    [[nodiscard]] static constexpr http1_client_request_prepare_result failure(
        http1_client_request_prepare_error error) noexcept {
        return http1_client_request_prepare_result(http1_client_request_prepare_failure(error));
    }

    [[nodiscard]] static http1_client_request_prepare_result prepared_streaming_content(
        std::string_view head, std::optional<std::uint64_t> length, bool gated,
        http1_client_exchange_state state_value) noexcept {
        return http1_client_request_prepare_result(prepared_http1_client_request(head,
            http1_client_request_content_plan(http1_client_streaming_request_content(length, gated)), std::move(state_value)));
    }

    [[nodiscard]] static http1_client_request_prepare_result prepared_without_content(
        std::string_view head, http1_client_exchange_state exchange_state) noexcept {
        return http1_client_request_prepare_result(prepared_http1_client_request(head,
            http1_client_request_content_plan(http1_client_request_without_content()),
            std::move(exchange_state)));
    }

    [[nodiscard]] static http1_client_request_prepare_result prepared_immediate_content(
        std::string_view head, std::string_view content_bytes,
        http1_client_exchange_state exchange_state) noexcept {
        return http1_client_request_prepare_result(prepared_http1_client_request(head,
            http1_client_request_content_plan(http1_client_immediate_request_content(content_bytes)),
            std::move(exchange_state)));
    }

    [[nodiscard]] static http1_client_request_prepare_result prepared_continue_gated_content(
        std::string_view head, std::string_view content_bytes,
        http1_client_exchange_state exchange_state) noexcept {
        return http1_client_request_prepare_result(prepared_http1_client_request(head,
            http1_client_request_content_plan(http1_client_continue_gated_request_content(content_bytes)),
            std::move(exchange_state)));
    }
};

}  // namespace ruvia::detail

namespace ruvia {
namespace {

constexpr std::string_view http11_request_line_suffix = " HTTP/1.1\r\n";
constexpr std::string_view host_prefix = "Host: ";
constexpr std::string_view content_length_prefix = "Content-Length: ";
constexpr std::string_view expect_prefix = "Expect: ";
constexpr std::string_view connection_close = "Connection: close\r\n";

[[nodiscard]] std::size_t authority_length(const http_origin_view& origin, bool force_port) noexcept {
    return origin.host().size() + ((force_port || !detail::http_origin_uses_default_port(origin))
                                          ? 1 + decimal_digits(origin.port())
                                          : 0);
}

void append_view(char*& cursor_value, std::string_view value) noexcept {
    if (!value.empty()) {
        std::memcpy(cursor_value, value.data(), value.size());
        cursor_value += value.size();
    }
}

void append_unsigned(char*& cursor_value, std::uint64_t value) noexcept {
    std::array<char, 32> digits;
    const auto [end, ec] = std::to_chars(digits.data(), digits.data() + digits.size(), value);
    if (ec == std::errc{}) {
        append_view(
            cursor_value, std::string_view(digits.data(), static_cast<std::size_t>(end - digits.data())));
    }
}

void append_authority(char*& cursor_value, const http_origin_view& origin, bool force_port) noexcept {
    append_view(cursor_value, origin.host());
    if (force_port || !detail::http_origin_uses_default_port(origin)) {
        *cursor_value++ = ':';
        append_unsigned(cursor_value, origin.port());
    }
}

void append_headers(char*& cursor_value, std::span<const http_header_view> headers) noexcept {
    for (const auto& header : headers) {
        append_view(cursor_value, header.name());
        append_view(cursor_value, ": ");
        append_view(cursor_value, header.value());
        append_view(cursor_value, crlf);
    }
}

[[nodiscard]] bool is_valid_http1_close_policy(http1_close_policy policy) noexcept {
    switch (policy) {
        case http1_close_policy::allow_reuse:
        case http1_close_policy::close_after_response:
            return true;
    }
    return false;
}

[[nodiscard]] bool is_valid_http_client_request_expectation(
    http_client_request_expectation expectation) noexcept {
    switch (expectation) {
        case http_client_request_expectation::none:
        case http_client_request_expectation::continue_value:
            return true;
    }
    return false;
}

[[nodiscard]] std::pmr::string own_offered_upgrade_protocols(
    std::span<const http_header_view> headers, std::pmr::memory_resource* resource) {
    std::pmr::string result(resource);
    for (const auto& header : headers) {
        if (!detail::http_ascii_equals_ignore_case(header.name(), "Upgrade")) {
            continue;
        }
        if (!result.empty()) {
            result.push_back(',');
        }
        result.append(header.value());
    }
    return result;
}

[[nodiscard]] http1_client_request_prepare_result prepare_request(const http_origin_view& origin,
    std::string_view method, std::string_view target, bool connect,
    std::span<const http_header_view> headers, http_client_request_content_view content,
    std::span<char> head_buffer, http1_client_request_wire_policy policy,
    std::pmr::memory_resource* resource, const http1_client_request_head_view* streaming = nullptr) {
    request_header_facts header_facts;
    http1_client_request_prepare_error error = http1_client_request_prepare_error::invalid_header;
    const auto* content_bytes = content.borrowed_bytes();
    const bool explicit_content = content_bytes != nullptr || streaming != nullptr;
    const auto length = streaming ? streaming->content_length_ : content_bytes ? std::optional<std::uint64_t>(content_bytes->value().size())
                                                                               : std::nullopt;
    const bool chunked = streaming && !length;
    if (!analyze_headers(headers, header_facts, error)) {
        return detail::http1_client_request_prepare_result_access::failure(error);
    }
    const bool expect_continue = policy.expectation_ == http_client_request_expectation::continue_value;
    const auto content_indication = explicit_content && (!length || *length != 0)
                                        ? http_request_content_indication::will_follow
                                        : http_request_content_indication::no_content;
    if (!http_client_expectation_is_valid(expect_continue, content_indication)) {
        return detail::http1_client_request_prepare_result_access::failure(
            http1_client_request_prepare_error::expectation_without_content);
    }
    if (explicit_content) {
        const auto content_semantics = http_request_content_semantics(method);
        if (content_semantics == http_request_content_semantics::forbidden) {
            return detail::http1_client_request_prepare_result_access::failure(
                http1_client_request_prepare_error::content_forbidden_for_method);
        }
        if (content_semantics == http_request_content_semantics::content_type_required &&
            !header_facts.has_content_type_) {
            return detail::http1_client_request_prepare_result_access::failure(
                http1_client_request_prepare_error::options_content_type_required);
        }
    }

    const bool generate_connection_close =
        policy.close_policy_ == http1_close_policy::close_after_response &&
        !header_facts.connection_options_.close();
    const auto effective_close_policy =
        header_facts.connection_options_.close() || generate_connection_close
            ? http1_close_policy::close_after_response
            : http1_close_policy::allow_reuse;
    constexpr std::string_view chunked_header = "Transfer-Encoding: chunked\r\n";
    const std::size_t generated_fields = 1 + (explicit_content ? 1 : 0) + (expect_continue ? 1 : 0) +
                                         (generate_connection_close ? 1 : 0);
    if (headers.size() > max_http_header_fields - generated_fields) {
        return detail::http1_client_request_prepare_result_access::failure(
            http1_client_request_prepare_error::too_many_headers);
    }

    const std::size_t target_bytes = connect ? authority_length(origin, true) : target.size();
    std::size_t head_bytes = 0;
    if (!add_head_bytes(head_bytes, method.size()) || !add_head_bytes(head_bytes, 1) ||
        !add_head_bytes(head_bytes, target_bytes) ||
        !add_head_bytes(head_bytes, http11_request_line_suffix.size()) ||
        !add_head_bytes(head_bytes, host_prefix.size()) ||
        !add_head_bytes(head_bytes, authority_length(origin, connect)) ||
        !add_head_bytes(head_bytes, crlf.size()) || !add_head_bytes(head_bytes, header_facts.wire_bytes_) ||
        (chunked && !add_head_bytes(head_bytes, chunked_header.size())) ||
        (explicit_content && !chunked &&
            (!add_head_bytes(head_bytes, content_length_prefix.size()) ||
                !add_head_bytes(head_bytes, decimal_digits(*length)) ||
                !add_head_bytes(head_bytes, crlf.size()))) ||
        (expect_continue &&
            (!add_head_bytes(head_bytes, expect_prefix.size()) ||
                !add_head_bytes(head_bytes, detail::http_continue_expectation_token.size()) ||
                !add_head_bytes(head_bytes, crlf.size()))) ||
        (generate_connection_close && !add_head_bytes(head_bytes, connection_close.size())) ||
        !add_head_bytes(head_bytes, crlf.size())) {
        return detail::http1_client_request_prepare_result_access::failure(
            http1_client_request_prepare_error::header_too_large);
    }
    if (head_buffer.size() < head_bytes) {
        return detail::http1_client_request_prepare_result_access::buffer_too_small(head_bytes);
    }

    // Upgrade is the only response-planning fact that cannot be reduced to a
    // fixed-size value. Own it before mutating the caller's output buffer so an
    // allocation failure leaves request preparation transactional.
    auto offered_upgrade_protocols = header_facts.upgrade_protocols_.has_protocol()
                                         ? own_offered_upgrade_protocols(headers, resource)
                                         : std::pmr::string(resource);

    char* cursor_value = head_buffer.data();
    append_view(cursor_value, method);
    *cursor_value++ = ' ';
    if (connect) {
        append_authority(cursor_value, origin, true);
    } else {
        append_view(cursor_value, target);
    }
    append_view(cursor_value, http11_request_line_suffix);
    append_view(cursor_value, host_prefix);
    append_authority(cursor_value, origin, connect);
    append_view(cursor_value, crlf);
    append_headers(cursor_value, headers);
    if (chunked) {
        append_view(cursor_value, chunked_header);
    } else if (explicit_content) {
        append_view(cursor_value, content_length_prefix);
        append_unsigned(cursor_value, *length);
        append_view(cursor_value, crlf);
    }
    if (expect_continue) {
        append_view(cursor_value, expect_prefix);
        append_view(cursor_value, detail::http_continue_expectation_token);
        append_view(cursor_value, crlf);
    }
    if (generate_connection_close) {
        append_view(cursor_value, connection_close);
    }
    append_view(cursor_value, crlf);

    const auto content_state = expect_continue
                                   ? detail::http1_client_initial_content_state::awaiting_continue
                                   : (explicit_content && (!length || *length != 0)
                                             ? detail::http1_client_initial_content_state::pending
                                             : detail::http1_client_initial_content_state::complete);
    auto exchange_state = detail::http1_client_request_prepare_result_access::exchange_state(
        connect ? http_known_method::connect : classify_http_method(method),
        header_facts.connection_options_, effective_close_policy, content_state,
        std::move(offered_upgrade_protocols));
    const auto head = std::string_view(head_buffer.data(), head_bytes);
    if (streaming) {
        return detail::http1_client_request_prepare_result_access::prepared_streaming_content(head, length, expect_continue, std::move(exchange_state));
    }
    if (!explicit_content) {
        return detail::http1_client_request_prepare_result_access::prepared_without_content(
            head, std::move(exchange_state));
    }
    if (expect_continue) {
        return detail::http1_client_request_prepare_result_access::prepared_continue_gated_content(
            head, content_bytes->value(), std::move(exchange_state));
    }
    return detail::http1_client_request_prepare_result_access::prepared_immediate_content(
        head, content_bytes->value(), std::move(exchange_state));
}

}  // namespace

std::string_view http1_client_request_prepare_error_message(
    http1_client_request_prepare_error error) noexcept {
    switch (error) {
        case http1_client_request_prepare_error::invalid_method:
            return "invalid HTTP/1 client request method";
        case http1_client_request_prepare_error::invalid_target:
            return "invalid HTTP/1 client request target";
        case http1_client_request_prepare_error::connect_requires_dedicated_entry:
            return "CONNECT requires the dedicated HTTP/1 client entry";
        case http1_client_request_prepare_error::invalid_connect_origin:
            return "invalid HTTP/1 CONNECT origin";
        case http1_client_request_prepare_error::invalid_header:
            return "invalid HTTP/1 client request header";
        case http1_client_request_prepare_error::too_many_headers:
            return "too many HTTP/1 client request headers";
        case http1_client_request_prepare_error::host_header_managed_by_writer:
            return "HTTP/1 client Host is managed by the writer";
        case http1_client_request_prepare_error::content_length_managed_by_writer:
            return "HTTP/1 client Content-Length is managed by the writer";
        case http1_client_request_prepare_error::transfer_encoding_unsupported:
            return "HTTP/1 client Transfer-Encoding is unsupported";
        case http1_client_request_prepare_error::trailer_section_unsupported:
            return "HTTP/1 client trailer sections are unsupported";
        case http1_client_request_prepare_error::expect_header_managed_by_writer:
            return "HTTP/1 client Expect is managed by the writer";
        case http1_client_request_prepare_error::invalid_connection:
            return "invalid HTTP/1 client Connection header";
        case http1_client_request_prepare_error::invalid_upgrade:
            return "invalid HTTP/1 client Upgrade header";
        case http1_client_request_prepare_error::upgrade_connection_option_required:
            return "HTTP/1 Upgrade requires Connection: Upgrade";
        case http1_client_request_prepare_error::te_connection_option_required:
            return "HTTP/1 TE requires Connection: TE";
        case http1_client_request_prepare_error::expectation_without_content:
            return "Continue expectation requires non-empty request content";
        case http1_client_request_prepare_error::content_forbidden_for_method:
            return "request method forbids content";
        case http1_client_request_prepare_error::options_content_type_required:
            return "OPTIONS content requires Content-Type";
        case http1_client_request_prepare_error::header_too_large:
            return "HTTP/1 client request header is too large";
        case http1_client_request_prepare_error::invalid_close_policy:
            return "invalid HTTP/1 client close policy";
        case http1_client_request_prepare_error::invalid_expectation:
            return "invalid HTTP/1 client request expectation";
    }
    return "invalid HTTP/1 client request";
}

http1_client_request_writer::http1_client_request_writer() noexcept
    : http1_client_request_writer(options_type{}) {}

http1_client_request_writer::http1_client_request_writer(options_type options) noexcept
    : resource_(detail::http_pmr_resource_or_default(options.resource_)) {}

http1_client_request_prepare_result http1_client_request_writer::prepare(const http_origin_view& origin,
    const http_client_request_view& request, std::span<char> head_buffer,
    http1_client_request_wire_policy policy) const {
    if (!is_valid_http1_close_policy(policy.close_policy_)) {
        return detail::http1_client_request_prepare_result_access::failure(
            http1_client_request_prepare_error::invalid_close_policy);
    }
    if (!is_valid_http_client_request_expectation(policy.expectation_)) {
        return detail::http1_client_request_prepare_result_access::failure(
            http1_client_request_prepare_error::invalid_expectation);
    }
    if (!is_valid_http_method_token(request.method_)) {
        return detail::http1_client_request_prepare_result_access::failure(
            http1_client_request_prepare_error::invalid_method);
    }
    if (request.method_ == "CONNECT") {
        return detail::http1_client_request_prepare_result_access::failure(
            http1_client_request_prepare_error::connect_requires_dedicated_entry);
    }
    if (!detail::is_valid_origin_or_asterisk_form_target(
            classify_http_method(request.method_), request.target_)) {
        return detail::http1_client_request_prepare_result_access::failure(
            http1_client_request_prepare_error::invalid_target);
    }
    return prepare_request(origin, request.method_, request.target_, false,
        static_cast<std::span<const http_header_view>>(request.headers_), request.content_, head_buffer,
        policy, resource_);
}

http1_client_request_prepare_result http1_client_request_writer::prepare_streaming(const http_origin_view& origin,
    const http1_client_request_head_view& request, std::span<char> head_buffer,
    http1_client_request_wire_policy policy) const {
    using access_type = detail::http1_client_request_prepare_result_access;
    if (!is_valid_http1_close_policy(policy.close_policy_)) {
        return access_type::failure(http1_client_request_prepare_error::invalid_close_policy);
    }
    if (!is_valid_http_client_request_expectation(policy.expectation_)) {
        return access_type::failure(http1_client_request_prepare_error::invalid_expectation);
    }
    if (!is_valid_http_method_token(request.method_)) {
        return access_type::failure(http1_client_request_prepare_error::invalid_method);
    }
    if (request.method_ == "CONNECT") {
        return access_type::failure(http1_client_request_prepare_error::connect_requires_dedicated_entry);
    }
    if (!detail::is_valid_origin_or_asterisk_form_target(classify_http_method(request.method_), request.target_)) {
        return access_type::failure(http1_client_request_prepare_error::invalid_target);
    }
    return prepare_request(origin, request.method_, request.target_, false, request.headers_, http_client_request_content_view::none(), head_buffer, policy, resource_, &request);
}

http1_client_request_prepare_result http1_client_request_writer::prepare_connect_udp(const http_origin_view& origin,
    borrowed_text target, std::span<const http_header_view> headers, std::span<char> head_buffer) const {
    std::pmr::vector<http_header_view> fields(resource_);
    fields.reserve(headers.size() + 3);
    for (const auto& field : headers) {
        if (http_ascii_equals_ignore_case(field.name(), "connection") || http_ascii_equals_ignore_case(field.name(), "upgrade")) {
            return detail::http1_client_request_prepare_result_access::failure(http1_client_request_prepare_error::invalid_upgrade);
        }
        fields.push_back(field);
    }
    fields.emplace_back("Connection", "Upgrade");
    fields.emplace_back("Upgrade", "connect-udp");
    fields.emplace_back("Host", origin.host());
    if ((validate_http_connect_udp_request({.version_ = http_protocol_version::http11, .method_ = "GET", .authority_ = origin.host(), .path_ = target.view(), .headers_ = fields}).index() != 0)) {
        return detail::http1_client_request_prepare_result_access::failure(http1_client_request_prepare_error::invalid_header);
    }
    fields.pop_back();
    return prepare(origin, {.method_ = "GET", .target_ = target, .headers_ = fields}, head_buffer);
}

http1_client_request_prepare_result http1_client_request_writer::prepare_connect(
    const http_origin_view& tunnel_origin, std::span<const http_header_view> headers,
    std::span<char> head_buffer, http1_client_request_wire_policy policy) const {
    if (!is_valid_http1_close_policy(policy.close_policy_)) {
        return detail::http1_client_request_prepare_result_access::failure(
            http1_client_request_prepare_error::invalid_close_policy);
    }
    if (!is_valid_http_client_request_expectation(policy.expectation_)) {
        return detail::http1_client_request_prepare_result_access::failure(
            http1_client_request_prepare_error::invalid_expectation);
    }
    if (tunnel_origin.port() == 0) {
        return detail::http1_client_request_prepare_result_access::failure(
            http1_client_request_prepare_error::invalid_connect_origin);
    }
    return prepare_request(tunnel_origin, "CONNECT", {}, true, headers,
        http_client_request_content_view::none(), head_buffer, policy, resource_);
}

http1_client_request_prepare_result http1_client_request_writer::prepare_connect(borrowed_text authority,
    std::span<const http_header_view> headers, std::span<char> head_buffer, http1_client_request_wire_policy policy) const {
    detail::request_target_view target;
    const auto parsed_value = detail::parse_http_authority(authority.view());
    if (!detail::parse_request_target(http_known_method::connect, authority.view(), target) || !parsed_value || !parsed_value->port() || *parsed_value->port() == 0) {
        return detail::http1_client_request_prepare_result_access::failure(http1_client_request_prepare_error::invalid_connect_origin);
    }
    return prepare_connect(http_origin_view::http({.host_ = parsed_value->host(), .port_ = *parsed_value->port()}), headers, head_buffer, policy);
}

}  // namespace ruvia
