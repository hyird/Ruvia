#include "ruvia/http/http1_client_request_writer.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string_view>

#include "ruvia/http/http1_chunked_framing.h"
#include "ruvia/http/http1_request_content_writer.h"
#include "ruvia/http/http1_response_head_error.h"
#include "ruvia/http/http1_server_semantics.h"
#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_connect_udp.h"
#include "ruvia/http/http_field_section.h"
#include "ruvia/http/http_field_syntax.h"
#include "ruvia/http/http_field_values.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_known_method.h"
#include "ruvia/http/http_number_format.h"
#include "ruvia/http/http_request_content_semantics.h"
#include "ruvia/http/http_request_trailers.h"

#include "field/http_connection_fields.h"
#include "field/http_field_kind.h"
#include "field/http_request_field_validation.h"

// What a caller-supplied request header section must satisfy before the writer
// will encode it, and the facts the encoder then needs from it: its field
// section budget, which singleton fields it already carries, and the
// connection and upgrade options it expresses.

#include <variant>

#include "ruvia/http/http1_client_response_parser.h"
#include "ruvia/http/http_base64.h"
#include "ruvia/http/http_protocol_managed_fields.h"
#include "ruvia/http/websocket_client_negotiation.h"

#include "parser/http_header_descriptor_storage.h"
#include "parser/http_request_target.h"
#include "response/http_response_header_state.h"
#include "websocket/http_websocket_handshake_fields.h"
#include "wire/http_wire_sink.h"

namespace ruvia::detail {

struct http1_client_request_field_facts final {
    // The caller's fields, charged by the shared field-section rule.
    http_field_section_budget section_;
    detail::request_field_state fields_{detail::request_field_framing::http1};
    detail::http_connection_options connection_options_;
    detail::http_upgrade_protocols upgrade_protocols_;
    std::string_view first_upgrade_value_{};
    std::size_t upgrade_value_bytes_{0};
    std::size_t upgrade_field_count_{0};
    bool has_te_{false};
    // The caller declared trailer fields; the writer requires chunked content.
    bool has_trailer_{false};
};

[[nodiscard]] bool analyze_http1_client_request_field(http_header_view header, detail::http_field_kind kind,
    http1_client_request_field_facts& facts, http1_client_request_prepare_error& error,
    http_request_field_source source = http_request_field_source::generated) noexcept;

[[nodiscard]] bool finish_http1_client_request_fields(
    const http1_client_request_field_facts& facts, http1_client_request_prepare_error& error) noexcept;

// Validate every field and collect the facts the encoder needs. Returns false
// with `error` set on the first field that cannot be sent.
[[nodiscard]] bool analyze_http1_client_request_fields(std::span<const http_header_view> headers,
    http1_client_request_field_facts& facts, http1_client_request_prepare_error& error,
    http_request_field_source source) noexcept;

}  // namespace ruvia::detail

namespace ruvia::detail {

struct http1_client_request_prepare_result_access final {
    using result_type = std::variant<prepared_http1_client_request, http1_client_request_buffer_too_small,
        http1_client_request_prepare_error>;

    [[nodiscard]] static http1_client_exchange_state exchange_state(http_known_method method,
        http1_close_policy close_policy, http1_client_initial_content_state content_state,
        std::string offered_upgrade_protocols) noexcept {
        return http1_client_exchange_state(
            method, close_policy, content_state, std::move(offered_upgrade_protocols));
    }

    [[nodiscard]] static constexpr result_type buffer_too_small(
        std::size_t required_head_bytes) noexcept {
        return result_type(http1_client_request_buffer_too_small(required_head_bytes));
    }

    [[nodiscard]] static constexpr result_type failure(
        http1_client_request_prepare_error error) noexcept {
        return result_type(error);
    }

    [[nodiscard]] static result_type prepared_streaming_content(
        std::string_view head, std::optional<std::uint64_t> length, bool gated,
        http1_client_exchange_state state_value) noexcept {
        return result_type(prepared_http1_client_request(head,
            http1_client_request_content_plan(http1_client_streaming_request_content(length, gated)), std::move(state_value)));
    }

    [[nodiscard]] static result_type prepared_without_content(
        std::string_view head, http1_client_exchange_state exchange_state) noexcept {
        return result_type(prepared_http1_client_request(head,
            http1_client_request_content_plan(http1_client_request_without_content()),
            std::move(exchange_state)));
    }

    [[nodiscard]] static result_type prepared_immediate_content(
        std::string_view head, std::string_view content_bytes,
        http1_client_exchange_state exchange_state) noexcept {
        return result_type(prepared_http1_client_request(head,
            http1_client_request_content_plan(http1_client_immediate_request_content(content_bytes)),
            std::move(exchange_state)));
    }

    [[nodiscard]] static result_type prepared_continue_gated_content(
        std::string_view head, std::string_view content_bytes,
        http1_client_exchange_state exchange_state) noexcept {
        return result_type(prepared_http1_client_request(head,
            http1_client_request_content_plan(http1_client_continue_gated_request_content(content_bytes)),
            std::move(exchange_state)));
    }
};

}  // namespace ruvia::detail

namespace ruvia {
namespace {

constexpr std::string_view crlf = "\r\n";
constexpr std::string_view http11_request_line_suffix = " HTTP/1.1\r\n";
constexpr std::string_view host_prefix = "Host: ";
constexpr std::string_view content_length_prefix = "Content-Length: ";
constexpr std::string_view expect_prefix = "Expect: ";
constexpr std::string_view connection_close = "Connection: close\r\n";
constexpr std::string_view chunked_header = "Transfer-Encoding: chunked\r\n";

void append_authority(detail::http_raw_wire_sink& sink, std::string_view host, std::string_view port) noexcept {
    sink.append(host);
    if (!port.empty()) {
        sink.append(':');
        sink.append(port);
    }
}

struct request_head_write_plan final {
    std::string_view host_;
    std::string_view port_;
    std::string_view method_;
    // "http://" or "https://" ahead of the authority for absolute-form;
    // empty for origin-form.
    std::string_view absolute_scheme_;
    std::string_view target_;
    std::span<const http_header_view> headers_;
    std::string_view content_length_;
    bool connect_;
    bool explicit_content_;
    bool expect_continue_;
    bool generate_connection_close_;
};

[[nodiscard]] char* write_request_head(char* output, const request_head_write_plan& plan) noexcept {
    detail::http_raw_wire_sink sink(output);
    sink.append(plan.method_);
    sink.append(' ');
    if (plan.connect_) {
        append_authority(sink, plan.host_, plan.port_);
    } else {
        if (!plan.absolute_scheme_.empty()) {
            sink.append(plan.absolute_scheme_);
            append_authority(sink, plan.host_, plan.port_);
        }
        sink.append(plan.target_);
    }
    sink.append(http11_request_line_suffix);
    sink.append(host_prefix);
    append_authority(sink, plan.host_, plan.port_);
    sink.append(crlf);
    for (const auto& header : plan.headers_) {
        detail::emit_http1_field_line(sink, header.name(), header.value());
    }
    if (plan.explicit_content_ && plan.content_length_.empty()) {
        sink.append(chunked_header);
    } else if (plan.explicit_content_) {
        sink.append(content_length_prefix);
        sink.append(plan.content_length_);
        sink.append(crlf);
    }
    if (plan.expect_continue_) {
        sink.append(expect_prefix);
        sink.append(detail::http_continue_expectation_token);
        sink.append(crlf);
    }
    if (plan.generate_connection_close_) {
        sink.append(connection_close);
    }
    sink.append(crlf);
    return sink.position();
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

[[nodiscard]] std::string own_offered_upgrade_protocols(
    std::span<const http_header_view> headers, const detail::http1_client_request_field_facts& facts) {
    if (facts.upgrade_field_count_ == 0) {
        return {};
    }
    if (facts.upgrade_field_count_ == 1) {
        return std::string(facts.first_upgrade_value_);
    }
    std::string result;
    result.reserve(facts.upgrade_value_bytes_);
    for (const auto& header : headers) {
        if (!http_ascii_equals_ignore_case(header.name(), "Upgrade")) {
            continue;
        }
        if (!result.empty()) {
            result.push_back(',');
        }
        result.append(header.value());
    }
    return result;
}

// Every request line, CONNECT included, rejects these wire policies first.
// CONNECT is always authority-form (RFC 9112 section 3.2.3).
[[nodiscard]] std::optional<http1_client_request_prepare_error> validate_wire_policy(
    http1_client_request_wire_policy policy, bool connect) noexcept {
    if (!is_valid_http1_close_policy(policy.close_policy_)) {
        return http1_client_request_prepare_error::invalid_close_policy;
    }
    if (!is_valid_http_client_request_expectation(policy.expectation_)) {
        return http1_client_request_prepare_error::invalid_expectation;
    }
    switch (policy.target_form_) {
        case http1_request_target_form::origin:
            return std::nullopt;
        case http1_request_target_form::absolute:
            if (!connect) {
                return std::nullopt;
            }
            break;
    }
    return http1_client_request_prepare_error::invalid_target_form;
}

[[nodiscard]] std::optional<http1_client_request_prepare_error> validate_non_connect_request_line(
    std::string_view method, std::string_view target, http1_client_request_wire_policy policy,
    http_known_method& known_method) noexcept {
    if (const auto error = validate_wire_policy(policy, false)) {
        return error;
    }
    if (!is_valid_http_method_token(method)) {
        return http1_client_request_prepare_error::invalid_method;
    }
    known_method = classify_http_method(method);
    if (known_method == http_known_method::connect) {
        return http1_client_request_prepare_error::connect_requires_dedicated_entry;
    }
    if (!detail::is_valid_origin_or_asterisk_form_target(known_method, target)) {
        return http1_client_request_prepare_error::invalid_target;
    }
    return std::nullopt;
}

[[nodiscard]] detail::http1_client_request_prepare_result_access::result_type prepare_request(const http_origin_view& origin,
    std::string_view method, http_known_method known_method, std::string_view target,
    std::span<const http_header_view> headers, http_client_request_content_view content,
    std::span<char> head_buffer, http1_client_request_wire_policy policy,
    const http1_client_request_head_view* streaming = nullptr,
    std::span<const http_header_view> connect_udp_headers = {}) {
    if (!detail::is_valid_http_request_field_source(policy.field_source_)) {
        return detail::http1_client_request_prepare_result_access::failure(
            http1_client_request_prepare_error::invalid_header);
    }
    const bool connect = known_method == http_known_method::connect;
    detail::http1_client_request_field_facts header_facts;
    http1_client_request_prepare_error error = http1_client_request_prepare_error::invalid_header;
    const auto* content_bytes = content.borrowed_bytes();
    const bool explicit_content = content_bytes != nullptr || streaming != nullptr;
    const auto length = streaming       ? (streaming->content_length_
                                                  ? std::optional<std::uint64_t>(streaming->content_length_->value())
                                                  : std::nullopt)
                        : content_bytes ? std::optional<std::uint64_t>(content_bytes->value().size())
                                        : std::nullopt;
    const bool chunked = streaming && !length;
    if (!detail::analyze_http1_client_request_fields(headers, header_facts, error, policy.field_source_)) {
        return detail::http1_client_request_prepare_result_access::failure(error);
    }
    // A sender that will generate trailer fields SHOULD declare them (RFC 9110
    // section 6.6.2); only chunked content has a trailer section.
    if (header_facts.has_trailer_ && !chunked) {
        return detail::http1_client_request_prepare_result_access::failure(
            http1_client_request_prepare_error::trailer_requires_chunked_content);
    }
    const bool expect_continue = policy.expectation_ == http_client_request_expectation::continue_value;
    const auto content_indication = explicit_content && (!length || *length != 0)
                                        ? http_request_content_indication::will_follow
                                        : http_request_content_indication::no_content;
    if (!http_client_expectation_is_valid(expect_continue, content_indication)) {
        return detail::http1_client_request_prepare_result_access::failure(
            http1_client_request_prepare_error::expectation_without_content);
    }
    if (content_indication == http_request_content_indication::will_follow) {
        const auto content_semantics = classify_http_request_content_semantics(method);
        if (content_semantics == http_request_content_semantics::forbidden) {
            return detail::http1_client_request_prepare_result_access::failure(
                http1_client_request_prepare_error::content_forbidden_for_method);
        }
        // RFC 9110 section 9.3.7 requires the media description when generating
        // OPTIONS content, not when forwarding an already received request.
        if (policy.field_source_ == http_request_field_source::generated &&
            content_semantics == http_request_content_semantics::content_type_required &&
            !header_facts.fields_.seen(detail::http_field_kind::content_type)) {
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

    // Reuse each decimal token in measurement and emission. CONNECT uses the
    // same port token for both its authority target and generated Host field.
    std::array<char, http_max_unsigned_decimal_size> port_buffer;
    const auto port_value = connect || origin.port() != (origin.scheme() == http_scheme::https ? std::uint16_t{443} : std::uint16_t{80})
                                ? http_format_unsigned(port_buffer, origin.port())
                                : std::string_view{};
    const auto authority_bytes = origin.host().size() + (port_value.empty() ? 0 : 1 + port_value.size());
    std::array<char, http_max_unsigned_decimal_size> content_length_buffer;
    const auto content_length_value = explicit_content && !chunked
                                          ? http_format_unsigned(content_length_buffer, *length)
                                          : std::string_view{};
    // absolute-form names the origin as the generated Host does, so the two
    // authorities agree (RFC 9112 section 3.2); OPTIONS * keeps only the
    // authority, with an empty path (section 3.2.4).
    const auto absolute_scheme = connect || policy.target_form_ != http1_request_target_form::absolute
                                     ? std::string_view{}
                                 : origin.scheme() == http_scheme::https ? std::string_view{"https://"}
                                                                         : std::string_view{"http://"};
    const auto emitted_target = !absolute_scheme.empty() && target == "*" ? std::string_view{} : target;
    const std::size_t target_bytes =
        connect ? authority_bytes
                : (absolute_scheme.empty() ? 0 : absolute_scheme.size() + authority_bytes) + emitted_target.size();
    // The caller's fields are already charged; add the request line and the
    // generated fields to the same section budget.
    auto head_budget = header_facts.section_;
    if (!head_budget.add(method.size()) || !head_budget.add(1) || !head_budget.add(target_bytes) ||
        !head_budget.add(http11_request_line_suffix.size()) ||
        !head_budget.add(host_prefix.size()) || !head_budget.add(authority_bytes) ||
        !head_budget.add(crlf.size()) || (chunked && !head_budget.add(chunked_header.size())) ||
        (explicit_content && !chunked &&
            (!head_budget.add(content_length_prefix.size()) ||
                !head_budget.add(content_length_value.size()) || !head_budget.add(crlf.size()))) ||
        (expect_continue &&
            (!head_budget.add(expect_prefix.size()) ||
                !head_budget.add(detail::http_continue_expectation_token.size()) ||
                !head_budget.add(crlf.size()))) ||
        (generate_connection_close && !head_budget.add(connection_close.size())) ||
        !head_budget.add(crlf.size())) {
        return detail::http1_client_request_prepare_result_access::failure(
            http1_client_request_prepare_error::header_too_large);
    }
    if (!connect_udp_headers.empty() &&
        validate_http_connect_udp_request_to_send({
                                                      .version_ = http_protocol_version::http11,
                                                      .method_ = "GET",
                                                      .authority_ = origin.host(),
                                                      .path_ = target,
                                                      .headers_ = connect_udp_headers,
                                                  })
            .has_value()) {
        return detail::http1_client_request_prepare_result_access::failure(
            http1_client_request_prepare_error::invalid_header);
    }
    const auto head_bytes = head_budget.bytes();
    if (head_buffer.size() < head_bytes) {
        return detail::http1_client_request_prepare_result_access::buffer_too_small(head_bytes);
    }

    // Upgrade is the only response-planning fact that cannot be reduced to a
    // fixed-size value. Own it before mutating the caller's output buffer so an
    // allocation failure leaves request preparation transactional.
    auto offered_upgrade_protocols = header_facts.upgrade_protocols_.has_protocol()
                                         ? own_offered_upgrade_protocols(headers, header_facts)
                                         : std::string();

    const auto content_state = expect_continue
                                   ? detail::http1_client_initial_content_state::awaiting_continue
                                   : (explicit_content && (!length || *length != 0)
                                             ? detail::http1_client_initial_content_state::pending
                                             : detail::http1_client_initial_content_state::complete);
    auto exchange_state = detail::http1_client_request_prepare_result_access::exchange_state(
        known_method, effective_close_policy, content_state,
        std::move(offered_upgrade_protocols));
    const request_head_write_plan write_plan{origin.host(), port_value, method, absolute_scheme, emitted_target, headers, content_length_value,
        connect, explicit_content, expect_continue, generate_connection_close};
    const auto output = head_buffer.first(head_bytes);
    // Descriptors and every borrowed string are consumed before the output is
    // overwritten when the caller's request storage aliases the head buffer.
    detail::write_http_wire(output,
        detail::http_wire_inputs_overlap(std::string_view(output.data(), output.size()), headers,
            {method, target, origin.host()}),
        [&write_plan](char* destination) noexcept { return write_request_head(destination, write_plan); });
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

// The one CONNECT rule set behind both prepare_connect() entries: wire-policy
// checks first, then the tunnel target (null when the caller's authority is
// invalid; port 0 is never a tunnel target), emitted in authority-form.
[[nodiscard]] detail::http1_client_request_prepare_result_access::result_type prepare_connect_request(
    const http_origin_view* tunnel_origin, std::span<const http_header_view> headers, std::span<char> head_buffer,
    http1_client_request_wire_policy policy) {
    if (const auto error = validate_wire_policy(policy, true)) {
        return detail::http1_client_request_prepare_result_access::failure(*error);
    }
    if (tunnel_origin == nullptr || tunnel_origin->port() == 0) {
        return detail::http1_client_request_prepare_result_access::failure(
            http1_client_request_prepare_error::invalid_connect_origin);
    }
    return prepare_request(*tunnel_origin, "CONNECT", http_known_method::connect, {}, headers,
        http_client_request_content_view::none(), head_buffer, policy);
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
        case http1_client_request_prepare_error::host_header_managed_by_writer:
            return "HTTP/1 client Host is managed by the writer";
        case http1_client_request_prepare_error::content_length_managed_by_writer:
            return "HTTP/1 client Content-Length is managed by the writer";
        case http1_client_request_prepare_error::transfer_encoding_unsupported:
            return "HTTP/1 client Transfer-Encoding is unsupported";
        case http1_client_request_prepare_error::trailer_requires_chunked_content:
            return "HTTP/1 client Trailer requires chunked request content";
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
        case http1_client_request_prepare_error::invalid_target_form:
            return "invalid HTTP/1 client request target form";
    }
    return "invalid HTTP/1 client request";
}

std::variant<prepared_http1_client_request, http1_client_request_buffer_too_small,
    http1_client_request_prepare_error>
http1_client_request_writer::prepare(const http_origin_view& origin,
    const http_client_request_view& request, std::span<char> head_buffer,
    http1_client_request_wire_policy policy) const {
    http_known_method known_method = http_known_method::unknown;
    if (const auto error = validate_non_connect_request_line(
            request.method_, request.target_, policy, known_method)) {
        return detail::http1_client_request_prepare_result_access::failure(*error);
    }
    return prepare_request(origin, request.method_, known_method, request.target_,
        static_cast<std::span<const http_header_view>>(request.headers_), request.content_, head_buffer,
        policy);
}

std::variant<prepared_http1_client_request, http1_client_request_buffer_too_small,
    http1_client_request_prepare_error>
http1_client_request_writer::prepare_streaming(const http_origin_view& origin,
    const http1_client_request_head_view& request, std::span<char> head_buffer,
    http1_client_request_wire_policy policy) const {
    http_known_method known_method = http_known_method::unknown;
    if (const auto error = validate_non_connect_request_line(
            request.method_, request.target_, policy, known_method)) {
        return detail::http1_client_request_prepare_result_access::failure(*error);
    }
    return prepare_request(origin, request.method_, known_method, request.target_, request.headers_,
        http_client_request_content_view::none(), head_buffer, policy, &request);
}

std::variant<prepared_http1_client_request, http1_client_request_buffer_too_small,
    http1_client_request_prepare_error>
http1_client_request_writer::prepare_connect_udp(const http_origin_view& origin,
    borrowed_text target, std::span<const http_header_view> headers, std::span<char> head_buffer,
    http1_client_request_wire_policy policy) const {
    http_known_method known_method = http_known_method::unknown;
    if (const auto error = validate_non_connect_request_line("GET", target.view(), policy, known_method)) {
        return detail::http1_client_request_prepare_result_access::failure(*error);
    }
    for (const auto& field : headers) {
        if (http_ascii_equals_ignore_case(field.name(), "connection")) {
            return detail::http1_client_request_prepare_result_access::failure(
                http1_client_request_prepare_error::invalid_connection);
        }
        if (http_ascii_equals_ignore_case(field.name(), "upgrade")) {
            return detail::http1_client_request_prepare_result_access::failure(
                http1_client_request_prepare_error::invalid_upgrade);
        }
    }
    detail::http_header_descriptor_storage<http_header_view> staged_fields;
    staged_fields.reserve(headers.size() + 3U);
    for (const auto& field : headers) {
        staged_fields.append(field);
    }
    staged_fields.append(
        http_header_view{detail::http_connection_field_name, detail::http_connection_upgrade_option});
    staged_fields.append(http_header_view{"Upgrade", "connect-udp"});
    staged_fields.append(http_header_view{"Host", origin.host()});
    const auto all_fields = staged_fields.view();
    const auto emitted_headers = all_fields.first(all_fields.size() - 1U);
    return prepare_request(origin, "GET", known_method, target.view(), emitted_headers,
        http_client_request_content_view::none(), head_buffer, policy, nullptr, all_fields);
}

std::variant<prepared_http1_client_request, http1_client_request_buffer_too_small,
    http1_client_request_prepare_error>
http1_client_request_writer::prepare_connect(
    const http_origin_view& tunnel_origin, std::span<const http_header_view> headers,
    std::span<char> head_buffer, http1_client_request_wire_policy policy) const {
    return prepare_connect_request(&tunnel_origin, headers, head_buffer, policy);
}

std::variant<prepared_http1_client_request, http1_client_request_buffer_too_small,
    http1_client_request_prepare_error>
http1_client_request_writer::prepare_connect(borrowed_text authority,
    std::span<const http_header_view> headers, std::span<char> head_buffer, http1_client_request_wire_policy policy) const {
    // RFC 9110 section 9.3.6: the tunnel destination needs a non-empty port,
    // and port zero is rejected by prepare_connect_request.
    const auto parsed_value = parse_http_authority(authority);
    if (!parsed_value || parsed_value->port_kind() != http_authority_port_kind::value) {
        return prepare_connect_request(nullptr, headers, head_buffer, policy);
    }
    const auto tunnel_origin =
        http_origin_view::http({.host_ = parsed_value->host(), .port_ = *parsed_value->port()});
    return prepare_connect_request(&tunnel_origin, headers, head_buffer, policy);
}

http1_response_head_plan prepare_http1_connect_response_head(
    const http_response& response, http_protocol_version version) {
    if ((version != http_protocol_version::http10 && version != http_protocol_version::http11) ||
        !response.status().is_successful() || response.file_body() || !response.body_bytes().empty()) {
        throw http1_response_head_error("invalid HTTP/1 CONNECT response head");
    }
    if (detail::response_has_known_header(
            response, detail::response_header_content_length | detail::response_header_transfer_encoding)) {
        throw http1_response_head_error("CONNECT response cannot contain message framing fields");
    }
    const auto body = plan_http_response_body(http_known_method::connect, response.status());
    const auto connection = version == http_protocol_version::http10
                                ? plan_http10_request_connection(true, false)
                                : http1_request_connection_plan::http11_close();
    return http1_close_delimited_response_stream_head_plan(body, connection);
}

}  // namespace ruvia

namespace ruvia::detail {

bool analyze_http1_client_request_field(http_header_view header, detail::http_field_kind kind,
    http1_client_request_field_facts& facts, http1_client_request_prepare_error& error,
    http_request_field_source source) noexcept {
    const auto name = header.name();
    const auto value = header.value();
    if (!is_valid_http_header_name(name) || !is_valid_http_header_value(value)) {
        error = http1_client_request_prepare_error::invalid_header;
        return false;
    }
    // The HTTP/1 writer owns Host and refuses message framing it does not
    // generate; the remaining semantics are version-independent.
    switch (kind) {
        case detail::http_field_kind::host:
            error = http1_client_request_prepare_error::host_header_managed_by_writer;
            return false;
        case detail::http_field_kind::transfer_encoding:
            error = http1_client_request_prepare_error::transfer_encoding_unsupported;
            return false;
        default:
            break;
    }
    switch (detail::validate_request_field(kind, value, detail::http_field_list_role::sender, facts.fields_, source)) {
        case detail::request_field_status::ok:
            break;
        case detail::request_field_status::invalid:
            error = http1_client_request_prepare_error::invalid_header;
            return false;
        case detail::request_field_status::managed_by_sender:
            error = kind == detail::http_field_kind::expect
                        ? http1_client_request_prepare_error::expect_header_managed_by_writer
                        : http1_client_request_prepare_error::content_length_managed_by_writer;
            return false;
    }
    if (kind == detail::http_field_kind::connection) {
        if (facts.connection_options_.parse_field(value, detail::http_connection_field_role::sender) !=
            detail::http_field_list_parse_status::ok) {
            error = http1_client_request_prepare_error::invalid_connection;
            return false;
        }
    } else if (kind == detail::http_field_kind::upgrade) {
        const auto role = source == http_request_field_source::forwarded
                              ? detail::http_field_list_role::recipient
                              : detail::http_field_list_role::sender;
        if (facts.upgrade_protocols_.parse_field(value, role) !=
            detail::http_field_list_parse_status::ok) {
            error = http1_client_request_prepare_error::invalid_upgrade;
            return false;
        }
        if (facts.upgrade_field_count_ == 0) {
            facts.first_upgrade_value_ = value;
        } else {
            ++facts.upgrade_value_bytes_;
        }
        facts.upgrade_value_bytes_ += value.size();
        ++facts.upgrade_field_count_;
    } else if (kind == detail::http_field_kind::te) {
        facts.has_te_ = true;
    } else if (kind == detail::http_field_kind::trailer) {
        facts.has_trailer_ = true;
    }

    if (!facts.section_.add_field(name, value)) {
        error = http1_client_request_prepare_error::header_too_large;
        return false;
    }
    return true;
}

bool finish_http1_client_request_fields(
    const http1_client_request_field_facts& facts, http1_client_request_prepare_error& error) noexcept {
    if (facts.upgrade_protocols_.has_field() && !facts.connection_options_.upgrade()) {
        error = http1_client_request_prepare_error::upgrade_connection_option_required;
        return false;
    }
    if (facts.has_te_ && !facts.connection_options_.te()) {
        error = http1_client_request_prepare_error::te_connection_option_required;
        return false;
    }
    return true;
}

bool analyze_http1_client_request_fields(std::span<const http_header_view> headers, http1_client_request_field_facts& facts,
    http1_client_request_prepare_error& error, http_request_field_source source) noexcept {
    for (const auto& header : headers) {
        if (!analyze_http1_client_request_field(header, detail::classify_http_field(header.name()), facts, error, source)) {
            return false;
        }
    }
    return finish_http1_client_request_fields(facts, error);
}

}  // namespace ruvia::detail

namespace ruvia {
namespace {

[[nodiscard]] char* write_chunked_trailers(char* output, std::span<const http_header_view> trailers) noexcept {
    detail::http_raw_wire_sink sink(output);
    sink.append(http1_last_chunk_prefix);
    for (const auto& field : trailers) {
        detail::emit_http1_field_line(sink, field.name(), field.value());
    }
    sink.append(http1_trailer_section_terminator);
    return sink.position();
}

}  // namespace

using error_type = http1_request_content_write_error;
http1_request_content_writer::http1_request_content_writer(const http1_client_streaming_request_content& plan) noexcept
    : length_(plan.content_length()),
      gated_(plan.continue_gated()) {}
void http1_request_content_writer::release_content() noexcept {
    gated_ = false;
}
void http1_request_content_writer::abort() noexcept {
    stopped_ = true;
    writing_ = false;
    finishing_ = false;
}
std::variant<http1_request_content_writer::chunk, error_type> http1_request_content_writer::plan_chunk(std::span<const char> payload_value) noexcept {
    if (stopped_ || finished_) {
        return error_type::stopped;
    }
    if (gated_) {
        return error_type::awaiting_continue;
    }
    if (writing_ || finishing_) {
        return error_type::write_pending;
    }
    if (payload_value.size() > std::numeric_limits<std::uint64_t>::max() - committed_) {
        return error_type::length_overflow;
    }
    if (length_ && payload_value.size() > *length_ - committed_) {
        return error_type::length_mismatch;
    }
    chunk result;
    result.payload_ = payload_value;
    if (!length_ && !payload_value.empty()) {
        http1_chunk_header header_value(payload_value.size());
        auto prefix = header_value.view();
        std::copy(prefix.begin(), prefix.end(), result.prefix_.begin());
        result.prefix_size_ = prefix.size();
        result.suffix_ = http1_chunk_data_terminator;
    }
    writing_ = true;
    pending_ = payload_value.size();
    return result;
}
void http1_request_content_writer::commit_chunk() {
    if (stopped_ || finished_ || !writing_) {
        throw std::logic_error("HTTP/1 request content chunk commit requires a pending chunk write");
    }
    committed_ += pending_;
    pending_ = 0;
    writing_ = false;
}
std::variant<std::string_view, error_type> http1_request_content_writer::plan_finish(std::span<char> buffer, std::span<const http_header_view> trailers) noexcept {
    if (stopped_ || finished_) {
        return error_type::stopped;
    }
    if (gated_) {
        return error_type::awaiting_continue;
    }
    if (writing_ || finishing_) {
        return error_type::write_pending;
    }
    if (length_ && committed_ != *length_) {
        return error_type::length_mismatch;
    }
    if (length_ && !trailers.empty()) {
        return error_type::trailers_require_chunked;
    }
    // The last-chunk line shares the section budget, which keeps the whole
    // encoded ending within the bounded wire snapshot.
    auto section = http_trailer_section_budget();
    (void)section.add(http1_last_chunk_prefix.size());
    for (const auto& field : trailers) {
        if (const auto error = detail::charge_http_request_trailer_field(section, field.name(), field.value())) {
            return *error == http_request_trailer_error::section_too_large ? error_type::header_too_large
                                                                           : error_type::invalid_trailer;
        }
    }
    const auto required = length_ ? std::size_t{0} : section.bytes();
    if (buffer.size() < required) {
        return error_type::output_too_small;
    }
    if (!length_) {
        const auto output = buffer.first(required);
        detail::write_http_wire(output,
            detail::http_wire_inputs_overlap(std::string_view(output.data(), output.size()), trailers),
            [trailers](char* destination) noexcept { return write_chunked_trailers(destination, trailers); });
    }
    finishing_ = true;
    return required ? std::string_view(buffer.data(), required) : std::string_view{};
}
void http1_request_content_writer::commit_finish() {
    if (stopped_ || finished_ || !finishing_) {
        throw std::logic_error("HTTP/1 request content finish commit requires a pending finish write");
    }
    finishing_ = false;
    finished_ = true;
}
}  // namespace ruvia

namespace ruvia {
namespace {
// The one application-header policy of the client handshake: the request
// writer owns every protocol-managed field (Host, framing, Expect and the
// Upgrade/Connection pair) and the handshake generates every Sec-WebSocket-*
// field.
bool is_reserved_handshake_header(std::string_view name, detail::http_field_kind kind) noexcept {
    switch (kind) {
        case detail::http_field_kind::sec_websocket_key:
        case detail::http_field_kind::sec_websocket_version:
        case detail::http_field_kind::sec_websocket_protocol:
        case detail::http_field_kind::sec_websocket_extensions:
            return true;
        case detail::http_field_kind::other:
            return detail::is_protocol_managed_request_field(kind, name) ||
                   http_ascii_equals_ignore_case(name, "sec-websocket-accept");
        default:
            return detail::is_protocol_managed_request_field(kind, name);
    }
}
}  // namespace

void validate_http1_websocket_client_handshake_configuration(std::span<const http_header_view> headers) {
    detail::http1_client_request_field_facts facts;
    http1_client_request_prepare_error error{};
    for (const auto& header : headers) {
        const auto kind = detail::classify_http_field(header.name());
        if (is_reserved_handshake_header(header.name(), kind)) {
            throw std::invalid_argument("invalid or reserved WebSocket handshake header");
        }
        if (!detail::analyze_http1_client_request_field(header, kind, facts, error)) {
            throw std::invalid_argument(http1_client_request_prepare_error_message(error).data());
        }
    }
    if (!detail::finish_http1_client_request_fields(facts, error)) {
        throw std::invalid_argument(http1_client_request_prepare_error_message(error).data());
    }
}

namespace {
template <typename header_type>
std::variant<prepared_http1_client_request, http1_client_request_buffer_too_small,
    http1_client_request_prepare_error>
prepare_websocket_request(const http_origin_view& origin, std::string_view target,
    std::span<const char, 24> encoded_key, const websocket_client_negotiation& negotiation,
    std::span<const header_type> headers, std::span<char> head_buffer, http_request_field_source source) {
    const auto negotiation_fields = negotiation.request_fields();
    detail::http_header_descriptor_storage<http_header_view> staged_headers;
    staged_headers.reserve(headers.size() + 3U + negotiation_fields.size());
    for (const auto& header : headers) {
        staged_headers.append(http_header_view{header.name(), header.value()});
    }
    staged_headers.append(http_header_view{"Upgrade", "websocket"});
    staged_headers.append(
        http_header_view{detail::http_connection_field_name, detail::http_connection_upgrade_option});
    staged_headers.append(
        http_header_view{"Sec-WebSocket-Key", std::string_view(encoded_key.data(), encoded_key.size())});
    for (const auto& field : negotiation_fields) {
        staged_headers.append(http_header_view{field.name(), field.value()});
    }
    return http1_client_request_writer()
        .prepare(origin, {.method_ = "GET", .target_ = target, .headers_ = staged_headers.view()}, head_buffer, {.field_source_ = source});
}

}  // namespace

std::variant<prepared_http1_client_request, http1_client_request_buffer_too_small,
    http1_client_request_prepare_error>
prepare_http1_websocket_client_request(const http_origin_view& origin, std::string_view target,
    std::span<const char, 24> encoded_key, const websocket_client_negotiation& negotiation,
    std::span<const http_header> headers, std::span<char> head_buffer, http_request_field_source source) {
    return prepare_websocket_request(origin, target, encoded_key, negotiation, headers, head_buffer, source);
}

std::variant<prepared_http1_client_request, http1_client_request_buffer_too_small,
    http1_client_request_prepare_error>
prepare_http1_websocket_client_request(const http_origin_view& origin, std::string_view target,
    std::span<const char, 24> encoded_key, const websocket_client_negotiation& negotiation,
    std::span<const http_header_view> headers, std::span<char> head_buffer, http_request_field_source source) {
    return prepare_websocket_request(origin, target, encoded_key, negotiation, headers, head_buffer, source);
}

std::variant<websocket_client_negotiation_result_view, websocket_client_handshake_error>
validate_http1_websocket_client_response(std::span<const char, 24> encoded_key,
    const websocket_client_negotiation& negotiation, const http1_parsed_client_response_head& response) {
    if (response.plan().protocol_upgrade() == nullptr ||
        response.head().status() != http_status::switching_protocols) {
        return websocket_client_handshake_error::response_status;
    }
    detail::websocket_accept_key_type expected{};
    detail::encode_websocket_accept(
        expected, std::string_view(encoded_key.data(), encoded_key.size()));
    std::size_t accepts = 0;
    bool accept_matches = false, has_upgrade = false, has_connection = false;
    for (const auto& header : response.head().headers()) {
        switch (detail::classify_http_field(header.name())) {
            case detail::http_field_kind::upgrade:
                has_upgrade = has_upgrade || detail::http_has_token(header.value(), "websocket");
                break;
            case detail::http_field_kind::connection:
                has_connection = has_connection ||
                                 detail::http_has_token(header.value(), detail::http_connection_upgrade_option);
                break;
            case detail::http_field_kind::other:
                if (http_ascii_equals_ignore_case(header.name(), "Sec-WebSocket-Accept")) {
                    ++accepts;
                    accept_matches = http_trim_ows(header.value()) ==
                                     std::string_view(expected.data(), expected.size());
                }
                break;
            default:
                break;
        }
    }
    if (accepts != 1 || !accept_matches) {
        return websocket_client_handshake_error::accept;
    }
    if (!has_upgrade) {
        return websocket_client_handshake_error::upgrade;
    }
    if (!has_connection) {
        return websocket_client_handshake_error::connection;
    }

    return detail::validate_websocket_client_selection(negotiation, response.head().headers(), false);
}
}  // namespace ruvia
