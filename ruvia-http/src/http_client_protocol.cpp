#include "http_client_protocol.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "borrowed_text.h"
#include "http_connection_fields.h"
#include "http_header.h"
#include "http_request.h"
#include "http_response_server.h"
#include "websocket_handshake.h"
namespace ruvia {
namespace {

[[nodiscard]] constexpr bool http_origin_uses_default_port(const http_origin_view& origin) noexcept {
    return origin.port() == detail::http_scheme_default_port(origin.scheme());
}

void validate_origin_host(std::string_view host) {
    if (host.empty()) {
        throw std::invalid_argument("HTTP origin host must not be empty");
    }
    if (!detail::is_valid_http_host(host)) {
        throw std::invalid_argument("HTTP origin host is invalid");
    }
}

std::uint16_t resolved_origin_port(http_scheme scheme, const http_origin_options& options) noexcept {
    if (options.port_.has_value()) {
        return options.port_->value();
    }
    return detail::http_scheme_default_port(scheme);
}

void remove_http_client_last_path_segment(std::string& path) noexcept {
    const auto slash = path.rfind('/');
    if (slash == std::string::npos) {
        path.clear();
        return;
    }
    path.erase(slash);
}

// `path` is always absolute: every caller builds it from a '/'-prefixed
// reference path or base directory, so only the '/'-led dot-segment cases of
// RFC 3986 section 5.2.4 can occur.
void normalize_http_client_absolute_path(std::string_view path, std::size_t extra_reserve, std::string& normalized) {
    normalized.clear();
    normalized.reserve((path.empty() ? 1U : path.size()) + extra_reserve);
    if (!path.empty() && path.find("/.") == std::string_view::npos) {
        normalized.append(path.data(), path.size());
        return;
    }
    auto remaining = path;
    while (!remaining.empty()) {
        // RFC 3986 section 5.2.4 deliberately moves one path segment at a
        // time. Empty segments are significant: reducing the path to a stack
        // of non-dot segments would turn "/a//." into "/a/" instead of the
        // required "/a//".
        if (remaining.starts_with("/./")) {
            remaining.remove_prefix(2);
        } else if (remaining == "/.") {
            remaining = "/";
        } else if (remaining.starts_with("/../")) {
            remaining.remove_prefix(3);
            remove_http_client_last_path_segment(normalized);
        } else if (remaining == "/..") {
            remaining = "/";
            remove_http_client_last_path_segment(normalized);
        } else {
            const auto next_slash = remaining.find('/', 1);
            const auto segment_bytes = next_slash == std::string_view::npos ? remaining.size() : next_slash;
            normalized.append(remaining.substr(0, segment_bytes));
            remaining.remove_prefix(segment_bytes);
        }
    }

    if (normalized.empty()) {
        normalized.push_back('/');
    }
}

// Merges the reference's path/query with the current origin-form target per
// RFC 3986 section 5.2 and validates the resolved origin-form target. Returns
// false when the location's path or the merged product is not a valid target.
// `reference` is the URI-reference with any scheme/authority prefix and the
// fragment already removed.
[[nodiscard]] bool resolve_http_client_redirect_path_and_query(bool has_authority, std::string_view reference, std::string_view current_target, std::string& resolved) {
    const auto query_at = reference.find('?');
    const bool has_reference_query = query_at != std::string_view::npos;
    const auto reference_path = has_reference_query ? reference.substr(0, query_at) : reference;
    const auto reference_query = has_reference_query ? reference.substr(query_at + 1) : std::string_view{};
    // Dot-segment removal must not hide malformed bytes in discarded segments.
    if (!detail::is_valid_uri_component(reference_path, true, false) || !detail::is_valid_uri_component(reference_query, true, true)) {
        return false;
    }

    static constexpr std::size_t inline_path_capacity = 256;
    std::array<char, inline_path_capacity> inline_merged_path{};
    std::string merged_path_storage;
    std::string_view merged_path;
    std::string_view selected_query;
    bool has_selected_query = has_reference_query;

    if (has_authority) {
        if (!reference_path.empty() && reference_path.front() != '/') {
            return false;
        }
        merged_path = reference_path.empty() ? std::string_view("/") : reference_path;
        selected_query = reference_query;
    } else {
        const auto base_query_at = current_target.find('?');
        const bool has_base_query = base_query_at != std::string_view::npos;
        const auto base_path = has_base_query ? current_target.substr(0, base_query_at) : current_target;
        const auto base_query = has_base_query ? current_target.substr(base_query_at + 1) : std::string_view{};

        if (reference_path.empty()) {
            merged_path = base_path;
            if (has_reference_query) {
                selected_query = reference_query;
            } else {
                has_selected_query = has_base_query;
                selected_query = base_query;
            }
        } else if (reference_path.front() == '/') {
            merged_path = reference_path;
            selected_query = reference_query;
        } else {
            const auto prefix_size = base_path.rfind('/') + 1U;
            const auto total_path_size = prefix_size + reference_path.size();
            if (total_path_size <= inline_merged_path.size()) {
                std::memcpy(inline_merged_path.data(), base_path.data(), prefix_size);
                std::memcpy(inline_merged_path.data() + prefix_size, reference_path.data(), reference_path.size());
                merged_path = std::string_view(inline_merged_path.data(), total_path_size);
            } else {
                merged_path_storage.reserve(total_path_size);
                merged_path_storage.append(base_path.data(), prefix_size);
                merged_path_storage.append(reference_path.data(), reference_path.size());
                merged_path = merged_path_storage;
            }
            selected_query = reference_query;
        }
    }

    const std::size_t query_extra = has_selected_query ? 1U + selected_query.size() : 0U;
    if (!has_authority && reference_path.empty()) {
        // RFC 3986 section 5.2.2 copies the base path for an empty reference path.
        resolved.clear();
        resolved.reserve(merged_path.size() + query_extra);
        resolved.append(merged_path.data(), merged_path.size());
    } else {
        normalize_http_client_absolute_path(merged_path, query_extra, resolved);
    }
    if (has_selected_query) {
        resolved.push_back('?');
        resolved.append(selected_query.data(), selected_query.size());
    }
    return is_valid_http_origin_form_target(resolved);
}

// The one redirect authority rule: no userinfo, a valid authority, and the
// origin comparison by effective port of the target scheme and URI host
// equivalence.
struct http_client_redirect_authority final {
    std::string_view host_;
    std::uint16_t port_;
    bool same_origin_;
};

[[nodiscard]] std::optional<http_client_redirect_authority> parse_http_client_redirect_authority(const http_origin_view& origin, http_scheme target_scheme, std::string_view authority) noexcept {
    if (authority.find('@') != std::string_view::npos) {
        return std::nullopt;
    }
    const auto parsed_value = parse_http_authority(authority);
    if (!parsed_value) {
        return std::nullopt;
    }
    const auto port = parsed_value->effective_port(detail::http_scheme_default_port(target_scheme));
    return http_client_redirect_authority{
        .host_ = parsed_value->host(),
        .port_ = port,
        .same_origin_ = target_scheme == origin.scheme() && port == origin.port() && detail::http_uri_host_equals(parsed_value->host(), origin.host()),
    };
}

}  // namespace

http_origin_view http_origin_view::http(http_origin_options options) {
    const auto host = options.host_.view();
    validate_origin_host(host);
    return http_origin_view(http_scheme::http, host, resolved_origin_port(http_scheme::http, options));
}

http_origin_view http_origin_view::https(http_origin_options options) {
    const auto host = options.host_.view();
    validate_origin_host(host);
    return http_origin_view(http_scheme::https, host, resolved_origin_port(http_scheme::https, options));
}

std::string make_http_origin_authority(const http_origin_view& origin) {
    const auto host = origin.host();
    const bool include_port = !http_origin_uses_default_port(origin);
    std::string authority;
    authority.reserve(host.size() + (include_port ? 6 : 0));
    authority.append(host);
    if (include_port) {
        authority.push_back(':');
        http_append_unsigned(authority, origin.port());
    }
    return authority;
}

http_client_response_header_lookup lookup_unique_http_client_response_header(const http_client_response_head& head, std::string_view name) noexcept {
    std::string_view found;
    bool seen = false;
    for (const auto& header : head.headers()) {
        if (!http_ascii_equals_ignore_case(header.name(), name)) {
            continue;
        }
        if (seen) {
            return {.status_ = http_unique_header_status::repeated};
        }
        seen = true;
        found = header.value();
    }
    if (!seen) {
        return {.status_ = http_unique_header_status::absent};
    }
    return {.status_ = http_unique_header_status::found, .value_ = found};
}

http_client_redirect_request_plan::http_client_redirect_request_plan(std::string_view method, http_client_redirect_content_disposition content_disposition) :
    method_(method),
    content_disposition_(content_disposition) {}

http_client_redirect_request_plan plan_http_client_redirect_request(const http_client_request_view& request, http_client_redirect_request_plan_options options) {
    if (options.status_ == http_status::see_other) {
        return http_client_redirect_request_plan(request.method_ == "HEAD" ? request.method_.view() : std::string_view("GET"), http_client_redirect_content_disposition::drop);
    }
    if ((options.status_ == http_status::moved_permanently || options.status_ == http_status::found) && request.method_ == "POST") {
        return http_client_redirect_request_plan("GET", http_client_redirect_content_disposition::drop);
    }
    return http_client_redirect_request_plan(request.method_.view(), http_client_redirect_content_disposition::preserve);
}

http_client_origin_authority_status classify_http_client_origin_authority(const http_origin_view& origin, std::string_view authority) noexcept {
    const auto parsed_value = parse_http_client_redirect_authority(origin, origin.scheme(), authority);
    if (!parsed_value) {
        return http_client_origin_authority_status::invalid_authority;
    }
    return parsed_value->same_origin_ ? http_client_origin_authority_status::same_origin : http_client_origin_authority_status::different_origin;
}

http_origin_view http_client_resolved_redirect::origin() const& {
    return scheme_ == http_scheme::https ? http_origin_view::https({.host_ = host(), .port_ = port_}) : http_origin_view::http({.host_ = host(), .port_ = port_});
}

std::variant<http_client_resolved_redirect, http_client_redirect_resolution_error> resolve_http_client_redirect_target(const http_origin_view& origin, http_client_redirect_target_options options) {
    const auto current_target = options.current_target_.view();
    auto location = options.location_.view();
    if (!is_valid_http_origin_form_target(current_target)) {
        return http_client_redirect_resolution_error::invalid_current_target;
    }

    location = http_trim_ows(location);
    if (const auto hash = location.find('#'); hash != std::string_view::npos) {
        if (!detail::is_valid_uri_component(location.substr(hash + 1), true, true)) {
            return http_client_redirect_resolution_error::invalid_location;
        }
        location = location.substr(0, hash);
    }

    bool has_authority = false;
    auto target_scheme = origin.scheme();
    std::string_view reference = location;
    const auto colon = reference.find(':');
    const auto first_path_or_query = reference.find_first_of("/?");
    if (colon != std::string_view::npos && (first_path_or_query == std::string_view::npos || colon < first_path_or_query)) {
        const auto scheme = reference.substr(0, colon);
        if (!detail::is_valid_uri_scheme(scheme)) {
            return http_client_redirect_resolution_error::invalid_location;
        }
        const bool is_https = http_ascii_equals_ignore_case(scheme, "https");
        if (!is_https && !http_ascii_equals_ignore_case(scheme, "http")) {
            return http_client_redirect_resolution_error::unsupported_scheme;
        }
        target_scheme = is_https ? http_scheme::https : http_scheme::http;
        reference.remove_prefix(colon + 1);
        if (!reference.starts_with("//")) {
            return http_client_redirect_resolution_error::invalid_location;
        }
        reference.remove_prefix(2);
        has_authority = true;
    } else if (reference.starts_with("//")) {
        reference.remove_prefix(2);
        has_authority = true;
    }

    std::string_view resolved_host = origin.host();
    auto port = origin.port();
    bool cross_origin = target_scheme != origin.scheme();
    if (has_authority) {
        const auto authority_end = reference.find_first_of("/?");
        const auto authority = authority_end == std::string_view::npos ? reference : reference.substr(0, authority_end);
        const auto parsed_value = parse_http_client_redirect_authority(origin, target_scheme, authority);
        if (!parsed_value) {
            return http_client_redirect_resolution_error::invalid_location;
        }
        resolved_host = parsed_value->host_;
        port = parsed_value->port_;
        cross_origin = !parsed_value->same_origin_;
        reference = authority_end == std::string_view::npos ? std::string_view{} : reference.substr(authority_end);
    }

    std::string resolved;
    if (!resolve_http_client_redirect_path_and_query(has_authority, reference, current_target, resolved)) {
        return http_client_redirect_resolution_error::invalid_location;
    }

    std::string host(resolved_host.data(), resolved_host.size());
    return http_client_resolved_redirect(target_scheme, std::move(host), port, std::move(resolved), cross_origin);
}

}  // namespace ruvia
// What a caller-supplied request header section must satisfy before the writer
// will encode it, and the facts the encoder then needs from it: its field
// section budget, which singleton fields it already carries, and the
// connection and upgrade options it expresses.

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

[[nodiscard]] bool analyze_http1_client_request_field(http_header_view header, detail::http_field_kind kind, http1_client_request_field_facts& facts, http1_client_request_prepare_error& error, http_request_field_source source = http_request_field_source::generated) noexcept;

[[nodiscard]] bool finish_http1_client_request_fields(const http1_client_request_field_facts& facts, http1_client_request_prepare_error& error) noexcept;

// Validate every field and collect the facts the encoder needs. Returns false
// with `error` set on the first field that cannot be sent.
[[nodiscard]] bool analyze_http1_client_request_fields(std::span<const http_header_view> headers, http1_client_request_field_facts& facts, http1_client_request_prepare_error& error, http_request_field_source source) noexcept;

struct http1_client_request_prepare_result_access final {
    using result_type = std::variant<prepared_http1_client_request, http1_client_request_buffer_too_small, http1_client_request_prepare_error>;

    [[nodiscard]] static http1_client_exchange_state exchange_state(http_known_method method, http1_close_policy close_policy, http1_client_initial_content_state content_state, std::string offered_upgrade_protocols) noexcept {
        return http1_client_exchange_state(method, close_policy, content_state, std::move(offered_upgrade_protocols));
    }

    [[nodiscard]] static constexpr result_type buffer_too_small(std::size_t required_head_bytes) noexcept {
        return result_type(http1_client_request_buffer_too_small(required_head_bytes));
    }

    [[nodiscard]] static constexpr result_type failure(http1_client_request_prepare_error error) noexcept {
        return result_type(error);
    }

    [[nodiscard]] static result_type prepared_streaming_content(std::string_view head, std::optional<std::uint64_t> length, bool gated, http1_client_exchange_state state_value) noexcept {
        return result_type(prepared_http1_client_request(head, http1_client_request_content_plan(http1_client_streaming_request_content(length, gated)), std::move(state_value)));
    }

    [[nodiscard]] static result_type prepared_without_content(std::string_view head, http1_client_exchange_state exchange_state) noexcept {
        return result_type(prepared_http1_client_request(head, http1_client_request_content_plan(http1_client_request_without_content()), std::move(exchange_state)));
    }

    [[nodiscard]] static result_type prepared_immediate_content(std::string_view head, std::string_view content_bytes, http1_client_exchange_state exchange_state) noexcept {
        return result_type(prepared_http1_client_request(head, http1_client_request_content_plan(http1_client_immediate_request_content(content_bytes)), std::move(exchange_state)));
    }

    [[nodiscard]] static result_type prepared_continue_gated_content(std::string_view head, std::string_view content_bytes, http1_client_exchange_state exchange_state) noexcept {
        return result_type(prepared_http1_client_request(head, http1_client_request_content_plan(http1_client_continue_gated_request_content(content_bytes)), std::move(exchange_state)));
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

[[nodiscard]] bool is_valid_http_client_request_expectation(http_client_request_expectation expectation) noexcept {
    switch (expectation) {
        case http_client_request_expectation::none:
        case http_client_request_expectation::continue_value:
            return true;
    }
    return false;
}

[[nodiscard]] std::string own_offered_upgrade_protocols(std::span<const http_header_view> headers, const detail::http1_client_request_field_facts& facts) {
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
[[nodiscard]] std::optional<http1_client_request_prepare_error> validate_wire_policy(http1_client_request_wire_policy policy, bool connect) noexcept {
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

[[nodiscard]] std::optional<http1_client_request_prepare_error> validate_non_connect_request_line(std::string_view method, std::string_view target, http1_client_request_wire_policy policy, http_known_method& known_method) noexcept {
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

[[nodiscard]] detail::http1_client_request_prepare_result_access::result_type prepare_request(const http_origin_view& origin, std::string_view method, http_known_method known_method, std::string_view target, std::span<const http_header_view> headers, http_client_request_content_view content, std::span<char> head_buffer, http1_client_request_wire_policy policy, const http1_client_request_head_view* streaming = nullptr, std::span<const http_header_view> connect_udp_headers = {}) {
    if (!detail::is_valid_http_request_field_source(policy.field_source_)) {
        return detail::http1_client_request_prepare_result_access::failure(http1_client_request_prepare_error::invalid_header);
    }
    const bool connect = known_method == http_known_method::connect;
    detail::http1_client_request_field_facts header_facts;
    http1_client_request_prepare_error error = http1_client_request_prepare_error::invalid_header;
    const auto* content_bytes = content.borrowed_bytes();
    const bool explicit_content = content_bytes != nullptr || streaming != nullptr;
    const auto length = streaming
        ? (streaming->content_length_ ? std::optional<std::uint64_t>(streaming->content_length_->value()) : std::nullopt)
        : content_bytes
        ? std::optional<std::uint64_t>(content_bytes->value().size())
        : std::nullopt;
    const bool chunked = streaming && !length;
    if (!detail::analyze_http1_client_request_fields(headers, header_facts, error, policy.field_source_)) {
        return detail::http1_client_request_prepare_result_access::failure(error);
    }
    // A sender that will generate trailer fields SHOULD declare them (RFC 9110
    // section 6.6.2); only chunked content has a trailer section.
    if (header_facts.has_trailer_ && !chunked) {
        return detail::http1_client_request_prepare_result_access::failure(http1_client_request_prepare_error::trailer_requires_chunked_content);
    }
    const bool expect_continue = policy.expectation_ == http_client_request_expectation::continue_value;
    const auto content_indication = explicit_content && (!length || *length != 0) ? http_request_content_indication::will_follow : http_request_content_indication::no_content;
    if (!http_client_expectation_is_valid(expect_continue, content_indication)) {
        return detail::http1_client_request_prepare_result_access::failure(http1_client_request_prepare_error::expectation_without_content);
    }
    if (content_indication == http_request_content_indication::will_follow) {
        const auto content_semantics = classify_http_request_content_semantics(method);
        if (content_semantics == http_request_content_semantics::forbidden) {
            return detail::http1_client_request_prepare_result_access::failure(http1_client_request_prepare_error::content_forbidden_for_method);
        }
        // RFC 9110 section 9.3.7 requires the media description when generating
        // OPTIONS content, not when forwarding an already received request.
        if (policy.field_source_ == http_request_field_source::generated && content_semantics == http_request_content_semantics::content_type_required && !header_facts.fields_.seen(detail::http_field_kind::content_type)) {
            return detail::http1_client_request_prepare_result_access::failure(http1_client_request_prepare_error::options_content_type_required);
        }
    }

    const bool generate_connection_close = policy.close_policy_ == http1_close_policy::close_after_response && !header_facts.connection_options_.close();
    const auto effective_close_policy = header_facts.connection_options_.close() || generate_connection_close ? http1_close_policy::close_after_response : http1_close_policy::allow_reuse;

    // Reuse each decimal token in measurement and emission. CONNECT uses the
    // same port token for both its authority target and generated Host field.
    std::array<char, http_max_unsigned_decimal_size> port_buffer;
    const auto port_value = connect || origin.port() != (origin.scheme() == http_scheme::https ? std::uint16_t{443} : std::uint16_t{80}) ? http_format_unsigned(port_buffer, origin.port()) : std::string_view{};
    const auto authority_bytes = origin.host().size() + (port_value.empty() ? 0 : 1 + port_value.size());
    std::array<char, http_max_unsigned_decimal_size> content_length_buffer;
    const auto content_length_value = explicit_content && !chunked ? http_format_unsigned(content_length_buffer, *length) : std::string_view{};
    // absolute-form names the origin as the generated Host does, so the two
    // authorities agree (RFC 9112 section 3.2); OPTIONS * keeps only the
    // authority, with an empty path (section 3.2.4).
    const auto absolute_scheme = connect || policy.target_form_ != http1_request_target_form::absolute
        ? std::string_view{}
        : origin.scheme() == http_scheme::https
        ? std::string_view{"https://"}
        : std::string_view{"http://"};
    const auto emitted_target = !absolute_scheme.empty() && target == "*" ? std::string_view{} : target;
    const std::size_t target_bytes = connect ? authority_bytes : (absolute_scheme.empty() ? 0 : absolute_scheme.size() + authority_bytes) + emitted_target.size();
    // The caller's fields are already charged; add the request line and the
    // generated fields to the same section budget.
    auto head_budget = header_facts.section_;
    if (!head_budget.add(method.size()) || !head_budget.add(1) || !head_budget.add(target_bytes) || !head_budget.add(http11_request_line_suffix.size()) || !head_budget.add(host_prefix.size()) || !head_budget.add(authority_bytes) || !head_budget.add(crlf.size()) || (chunked && !head_budget.add(chunked_header.size())) || (explicit_content && !chunked && (!head_budget.add(content_length_prefix.size()) || !head_budget.add(content_length_value.size()) || !head_budget.add(crlf.size()))) || (expect_continue && (!head_budget.add(expect_prefix.size()) || !head_budget.add(detail::http_continue_expectation_token.size()) || !head_budget.add(crlf.size()))) || (generate_connection_close && !head_budget.add(connection_close.size())) || !head_budget.add(crlf.size())) {
        return detail::http1_client_request_prepare_result_access::failure(http1_client_request_prepare_error::header_too_large);
    }
    if (!connect_udp_headers.empty() && validate_http_connect_udp_request_to_send({.version_ = http_protocol_version::http11, .method_ = "GET", .authority_ = origin.host(), .path_ = target, .headers_ = connect_udp_headers}).has_value()) {
        return detail::http1_client_request_prepare_result_access::failure(http1_client_request_prepare_error::invalid_header);
    }
    const auto head_bytes = head_budget.bytes();
    if (head_buffer.size() < head_bytes) {
        return detail::http1_client_request_prepare_result_access::buffer_too_small(head_bytes);
    }

    // Upgrade is the only response-planning fact that cannot be reduced to a
    // fixed-size value. Own it before mutating the caller's output buffer so an
    // allocation failure leaves request preparation transactional.
    auto offered_upgrade_protocols = header_facts.upgrade_protocols_.has_protocol() ? own_offered_upgrade_protocols(headers, header_facts) : std::string();

    const auto content_state = expect_continue ? detail::http1_client_initial_content_state::awaiting_continue : (explicit_content && (!length || *length != 0) ? detail::http1_client_initial_content_state::pending : detail::http1_client_initial_content_state::complete);
    auto exchange_state = detail::http1_client_request_prepare_result_access::exchange_state(known_method, effective_close_policy, content_state, std::move(offered_upgrade_protocols));
    const request_head_write_plan write_plan{origin.host(), port_value, method, absolute_scheme, emitted_target, headers, content_length_value, connect, explicit_content, expect_continue, generate_connection_close};
    const auto output = head_buffer.first(head_bytes);
    // Descriptors and every borrowed string are consumed before the output is
    // overwritten when the caller's request storage aliases the head buffer.
    detail::write_http_wire(output, detail::http_wire_inputs_overlap(std::string_view(output.data(), output.size()), headers, {method, target, origin.host()}), [&write_plan](char* destination) noexcept {
        return write_request_head(destination, write_plan);
    });
    const auto head = std::string_view(head_buffer.data(), head_bytes);
    if (streaming) {
        return detail::http1_client_request_prepare_result_access::prepared_streaming_content(head, length, expect_continue, std::move(exchange_state));
    }
    if (!explicit_content) {
        return detail::http1_client_request_prepare_result_access::prepared_without_content(head, std::move(exchange_state));
    }
    if (expect_continue) {
        return detail::http1_client_request_prepare_result_access::prepared_continue_gated_content(head, content_bytes->value(), std::move(exchange_state));
    }
    return detail::http1_client_request_prepare_result_access::prepared_immediate_content(head, content_bytes->value(), std::move(exchange_state));
}

// The one CONNECT rule set behind both prepare_connect() entries: wire-policy
// checks first, then the tunnel target (null when the caller's authority is
// invalid; port 0 is never a tunnel target), emitted in authority-form.
[[nodiscard]] detail::http1_client_request_prepare_result_access::result_type prepare_connect_request(const http_origin_view* tunnel_origin, std::span<const http_header_view> headers, std::span<char> head_buffer, http1_client_request_wire_policy policy) {
    if (const auto error = validate_wire_policy(policy, true)) {
        return detail::http1_client_request_prepare_result_access::failure(*error);
    }
    if (tunnel_origin == nullptr || tunnel_origin->port() == 0) {
        return detail::http1_client_request_prepare_result_access::failure(http1_client_request_prepare_error::invalid_connect_origin);
    }
    return prepare_request(*tunnel_origin, "CONNECT", http_known_method::connect, {}, headers, http_client_request_content_view::none(), head_buffer, policy);
}

}  // namespace

std::string_view http1_client_request_prepare_error_message(http1_client_request_prepare_error error) noexcept {
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

std::variant<prepared_http1_client_request, http1_client_request_buffer_too_small, http1_client_request_prepare_error> http1_client_request_writer::prepare(const http_origin_view& origin, const http_client_request_view& request, std::span<char> head_buffer, http1_client_request_wire_policy policy) const {
    http_known_method known_method = http_known_method::unknown;
    if (const auto error = validate_non_connect_request_line(request.method_, request.target_, policy, known_method)) {
        return detail::http1_client_request_prepare_result_access::failure(*error);
    }
    return prepare_request(origin, request.method_, known_method, request.target_, static_cast<std::span<const http_header_view>>(request.headers_), request.content_, head_buffer, policy);
}

std::variant<prepared_http1_client_request, http1_client_request_buffer_too_small, http1_client_request_prepare_error> http1_client_request_writer::prepare_streaming(const http_origin_view& origin, const http1_client_request_head_view& request, std::span<char> head_buffer, http1_client_request_wire_policy policy) const {
    http_known_method known_method = http_known_method::unknown;
    if (const auto error = validate_non_connect_request_line(request.method_, request.target_, policy, known_method)) {
        return detail::http1_client_request_prepare_result_access::failure(*error);
    }
    return prepare_request(origin, request.method_, known_method, request.target_, request.headers_, http_client_request_content_view::none(), head_buffer, policy, &request);
}

std::variant<prepared_http1_client_request, http1_client_request_buffer_too_small, http1_client_request_prepare_error> http1_client_request_writer::prepare_connect_udp(const http_origin_view& origin, borrowed_text target, std::span<const http_header_view> headers, std::span<char> head_buffer, http1_client_request_wire_policy policy) const {
    http_known_method known_method = http_known_method::unknown;
    if (const auto error = validate_non_connect_request_line("GET", target.view(), policy, known_method)) {
        return detail::http1_client_request_prepare_result_access::failure(*error);
    }
    for (const auto& field : headers) {
        if (http_ascii_equals_ignore_case(field.name(), "connection")) {
            return detail::http1_client_request_prepare_result_access::failure(http1_client_request_prepare_error::invalid_connection);
        }
        if (http_ascii_equals_ignore_case(field.name(), "upgrade")) {
            return detail::http1_client_request_prepare_result_access::failure(http1_client_request_prepare_error::invalid_upgrade);
        }
    }
    detail::http_header_descriptor_storage<http_header_view> staged_fields;
    staged_fields.reserve(headers.size() + 3U);
    for (const auto& field : headers) {
        staged_fields.append(field);
    }
    staged_fields.append(http_header_view{detail::http_connection_field_name, detail::http_connection_upgrade_option});
    staged_fields.append(http_header_view{"Upgrade", "connect-udp"});
    staged_fields.append(http_header_view{"Host", origin.host()});
    const auto all_fields = staged_fields.view();
    const auto emitted_headers = all_fields.first(all_fields.size() - 1U);
    return prepare_request(origin, "GET", known_method, target.view(), emitted_headers, http_client_request_content_view::none(), head_buffer, policy, nullptr, all_fields);
}

std::variant<prepared_http1_client_request, http1_client_request_buffer_too_small, http1_client_request_prepare_error> http1_client_request_writer::prepare_connect(const http_origin_view& tunnel_origin, std::span<const http_header_view> headers, std::span<char> head_buffer, http1_client_request_wire_policy policy) const {
    return prepare_connect_request(&tunnel_origin, headers, head_buffer, policy);
}

std::variant<prepared_http1_client_request, http1_client_request_buffer_too_small, http1_client_request_prepare_error> http1_client_request_writer::prepare_connect(borrowed_text authority, std::span<const http_header_view> headers, std::span<char> head_buffer, http1_client_request_wire_policy policy) const {
    // RFC 9110 section 9.3.6: the tunnel destination needs a non-empty port,
    // and port zero is rejected by prepare_connect_request.
    const auto parsed_value = parse_http_authority(authority);
    if (!parsed_value || parsed_value->port_kind() != http_authority_port_kind::value) {
        return prepare_connect_request(nullptr, headers, head_buffer, policy);
    }
    const auto tunnel_origin = http_origin_view::http({.host_ = parsed_value->host(), .port_ = *parsed_value->port()});
    return prepare_connect_request(&tunnel_origin, headers, head_buffer, policy);
}

http1_response_head_plan prepare_http1_connect_response_head(const http_response& response, http_protocol_version version) {
    if ((version != http_protocol_version::http10 && version != http_protocol_version::http11) || !response.status().is_successful() || response.file_body() || !response.body_bytes().empty()) {
        throw http1_response_head_error("invalid HTTP/1 CONNECT response head");
    }
    if (detail::response_has_known_header(response, detail::response_header_content_length | detail::response_header_transfer_encoding)) {
        throw http1_response_head_error("CONNECT response cannot contain message framing fields");
    }
    const auto body = plan_http_response_body(http_known_method::connect, response.status());
    const auto connection = version == http_protocol_version::http10 ? plan_http10_request_connection(true, false) : http1_request_connection_plan::http11_close();
    return http1_close_delimited_response_stream_head_plan(body, connection);
}

}  // namespace ruvia

namespace ruvia::detail {

bool analyze_http1_client_request_field(http_header_view header, detail::http_field_kind kind, http1_client_request_field_facts& facts, http1_client_request_prepare_error& error, http_request_field_source source) noexcept {
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
            error = kind == detail::http_field_kind::expect ? http1_client_request_prepare_error::expect_header_managed_by_writer : http1_client_request_prepare_error::content_length_managed_by_writer;
            return false;
    }
    if (kind == detail::http_field_kind::connection) {
        if (facts.connection_options_.parse_field(value, detail::http_connection_field_role::sender) != detail::http_field_list_parse_status::ok) {
            error = http1_client_request_prepare_error::invalid_connection;
            return false;
        }
    } else if (kind == detail::http_field_kind::upgrade) {
        const auto role = source == http_request_field_source::forwarded ? detail::http_field_list_role::recipient : detail::http_field_list_role::sender;
        if (facts.upgrade_protocols_.parse_field(value, role) != detail::http_field_list_parse_status::ok) {
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

bool finish_http1_client_request_fields(const http1_client_request_field_facts& facts, http1_client_request_prepare_error& error) noexcept {
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

bool analyze_http1_client_request_fields(std::span<const http_header_view> headers, http1_client_request_field_facts& facts, http1_client_request_prepare_error& error, http_request_field_source source) noexcept {
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
http1_request_content_writer::http1_request_content_writer(const http1_client_streaming_request_content& plan) noexcept :
    length_(plan.content_length()),
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
            return *error == http_request_trailer_error::section_too_large ? error_type::header_too_large : error_type::invalid_trailer;
        }
    }
    const auto required = length_ ? std::size_t{0} : section.bytes();
    if (buffer.size() < required) {
        return error_type::output_too_small;
    }
    if (!length_) {
        const auto output = buffer.first(required);
        detail::write_http_wire(output, detail::http_wire_inputs_overlap(std::string_view(output.data(), output.size()), trailers), [trailers](char* destination) noexcept {
            return write_chunked_trailers(destination, trailers);
        });
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
            return detail::is_protocol_managed_request_field(kind, name) || http_ascii_equals_ignore_case(name, "sec-websocket-accept");
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
std::variant<prepared_http1_client_request, http1_client_request_buffer_too_small, http1_client_request_prepare_error> prepare_websocket_request(const http_origin_view& origin, std::string_view target, std::span<const char, 24> encoded_key, const websocket_client_negotiation& negotiation, std::span<const header_type> headers, std::span<char> head_buffer, http_request_field_source source) {
    const auto negotiation_fields = negotiation.request_fields();
    detail::http_header_descriptor_storage<http_header_view> staged_headers;
    staged_headers.reserve(headers.size() + 3U + negotiation_fields.size());
    for (const auto& header : headers) {
        staged_headers.append(http_header_view{header.name(), header.value()});
    }
    staged_headers.append(http_header_view{"Upgrade", "websocket"});
    staged_headers.append(http_header_view{detail::http_connection_field_name, detail::http_connection_upgrade_option});
    staged_headers.append(http_header_view{"Sec-WebSocket-Key", std::string_view(encoded_key.data(), encoded_key.size())});
    for (const auto& field : negotiation_fields) {
        staged_headers.append(http_header_view{field.name(), field.value()});
    }
    return http1_client_request_writer().prepare(origin, {.method_ = "GET", .target_ = target, .headers_ = staged_headers.view()}, head_buffer, {.field_source_ = source});
}

}  // namespace

std::variant<prepared_http1_client_request, http1_client_request_buffer_too_small, http1_client_request_prepare_error> prepare_http1_websocket_client_request(const http_origin_view& origin, std::string_view target, std::span<const char, 24> encoded_key, const websocket_client_negotiation& negotiation, std::span<const http_header> headers, std::span<char> head_buffer, http_request_field_source source) {
    return prepare_websocket_request(origin, target, encoded_key, negotiation, headers, head_buffer, source);
}

std::variant<prepared_http1_client_request, http1_client_request_buffer_too_small, http1_client_request_prepare_error> prepare_http1_websocket_client_request(const http_origin_view& origin, std::string_view target, std::span<const char, 24> encoded_key, const websocket_client_negotiation& negotiation, std::span<const http_header_view> headers, std::span<char> head_buffer, http_request_field_source source) {
    return prepare_websocket_request(origin, target, encoded_key, negotiation, headers, head_buffer, source);
}

std::variant<websocket_client_negotiation_result_view, websocket_client_handshake_error> validate_http1_websocket_client_response(std::span<const char, 24> encoded_key, const websocket_client_negotiation& negotiation, const http1_parsed_client_response_head& response) {
    if (response.plan().protocol_upgrade() == nullptr || response.head().status() != http_status::switching_protocols) {
        return websocket_client_handshake_error::response_status;
    }
    detail::websocket_accept_key_type expected{};
    detail::encode_websocket_accept(expected, std::string_view(encoded_key.data(), encoded_key.size()));
    std::size_t accepts = 0;
    bool accept_matches = false, has_upgrade = false, has_connection = false;
    for (const auto& header : response.head().headers()) {
        switch (detail::classify_http_field(header.name())) {
            case detail::http_field_kind::upgrade:
                has_upgrade = has_upgrade || detail::http_has_token(header.value(), "websocket");
                break;
            case detail::http_field_kind::connection:
                has_connection = has_connection || detail::http_has_token(header.value(), detail::http_connection_upgrade_option);
                break;
            case detail::http_field_kind::other:
                if (http_ascii_equals_ignore_case(header.name(), "Sec-WebSocket-Accept")) {
                    ++accepts;
                    accept_matches = http_trim_ows(header.value()) == std::string_view(expected.data(), expected.size());
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

namespace ruvia::detail {

struct http1_client_response_parse_result_access final {
    [[nodiscard]] static http1_client_response_parse_result need_more() noexcept {
        return http1_client_response_parse_result(http1_client_response_need_more());
    }

    [[nodiscard]] static http1_client_response_parse_result failure(http1_client_response_parse_error error) noexcept {
        return http1_client_response_parse_result(error);
    }

    [[nodiscard]] static http1_client_response_parse_result parsed(http_client_response_head head, http1_client_response_plan plan, std::size_t consumed_bytes) noexcept {
        return http1_client_response_parse_result(http1_parsed_client_response_head(std::move(head), std::move(plan), consumed_bytes));
    }
};

}  // namespace ruvia::detail

namespace ruvia {

namespace {

[[nodiscard]] constexpr detail::http1_client_request_content_phase receive_continue(detail::http1_client_request_content_phase phase) noexcept {
    switch (phase) {
        case detail::http1_client_request_content_phase::awaiting_continue:
        case detail::http1_client_request_content_phase::content_released_awaiting_continue:
            return detail::http1_client_request_content_phase::continue_received;
        case detail::http1_client_request_content_phase::content_complete_awaiting_continue:
            return detail::http1_client_request_content_phase::continue_received_content_complete;
        case detail::http1_client_request_content_phase::content_complete:
        case detail::http1_client_request_content_phase::content_pending:
        case detail::http1_client_request_content_phase::continue_received:
        case detail::http1_client_request_content_phase::continue_received_content_complete:
            return phase;
    }
    return phase;
}

}  // namespace

std::string_view http1_client_response_parse_error_message(http1_client_response_parse_error error) noexcept {
    switch (error) {
        case http1_client_response_parse_error::header_too_large:
            return "response header is too large";
        case http1_client_response_parse_error::invalid_status_line:
            return "invalid response status line";
        case http1_client_response_parse_error::unsupported_http_version:
            return "unsupported response HTTP version";
        case http1_client_response_parse_error::invalid_status_code:
            return "invalid response status code";
        case http1_client_response_parse_error::invalid_reason_phrase:
            return "invalid response reason phrase";
        case http1_client_response_parse_error::invalid_header:
            return "invalid response header";
        case http1_client_response_parse_error::invalid_connection:
            return "invalid response Connection header";
        case http1_client_response_parse_error::invalid_upgrade:
            return "invalid response Upgrade header";
        case http1_client_response_parse_error::invalid_content_length:
            return "invalid response Content-Length";
        case http1_client_response_parse_error::conflicting_content_length:
            return "conflicting response Content-Length";
        case http1_client_response_parse_error::invalid_transfer_encoding:
            return "invalid response Transfer-Encoding";
        case http1_client_response_parse_error::unsupported_transfer_encoding:
            return "unsupported response transfer coding";
        case http1_client_response_parse_error::content_length_and_transfer_encoding:
            return "response has both Content-Length and Transfer-Encoding";
        case http1_client_response_parse_error::invalid_protocol_switch:
            return "invalid Switching Protocols response";
    }
    return "invalid HTTP/1 response";
}

detail::http1_client_request_content_phase http1_client_response_parser::initial_request_content_phase(const http1_client_exchange_state& state_value) noexcept {
    switch (detail::http1_client_exchange_state_access::content_state(state_value)) {
        case detail::http1_client_initial_content_state::complete:
            return detail::http1_client_request_content_phase::content_complete;
        case detail::http1_client_initial_content_state::pending:
            return detail::http1_client_request_content_phase::content_pending;
        case detail::http1_client_initial_content_state::awaiting_continue:
            return detail::http1_client_request_content_phase::awaiting_continue;
    }
    return detail::http1_client_request_content_phase::content_complete;
}

http1_client_response_parser::http1_client_response_parser(http1_client_exchange_state exchange_state) noexcept :
    exchange_state_(std::move(exchange_state)),
    request_content_phase_(initial_request_content_phase(exchange_state_)) {}

http1_client_response_parse_result http1_client_response_parser::parse(std::string_view buffer) {
    if (phase_ != phase_type::await_response) {
        throw std::logic_error("HTTP/1 client response parser is already terminal");
    }
    const auto fail = [this](http1_client_response_parse_error error) noexcept {
        phase_ = phase_type::failed;
        return detail::http1_client_response_parse_result_access::failure(error);
    };

    // The status line is validated once its line feed arrives, so a non-HTTP
    // peer or a bare LF fails without waiting for the whole head. Bytes before
    // header_scan_offset_ held no line feed in an earlier call.
    if (!status_line_) {
        const auto line_feed = buffer.substr(0, max_http_header_bytes).find('\n', header_scan_offset_);
        if (line_feed != std::string_view::npos) {
            if (line_feed == 0 || buffer[line_feed - 1] != '\r') {
                return fail(http1_client_response_parse_error::invalid_status_line);
            }
            const auto status_line = detail::parse_http1_client_status_line(buffer.substr(0, line_feed - 1));
            if (status_line.index() != 0) {
                return fail(std::get<1>(status_line));
            }
            status_line_ = std::get<0>(status_line);
            status_line_end_ = line_feed - 1;
        }
    }

    const auto header_bytes = detail::find_http_header_end(buffer, header_scan_offset_);
    if (header_bytes == std::string_view::npos) {
        // A field line ending in a bare LF can never complete the
        // CRLF-delimited head (RFC 9112 section 2.2).
        if (status_line_ && detail::has_http_bare_line_feed(buffer, std::max(status_line_end_ + 2, header_scan_offset_))) {
            return fail(http1_client_response_parse_error::invalid_header);
        }
        if (buffer.size() >= max_http_header_bytes) {
            return fail(http1_client_response_parse_error::header_too_large);
        }
        header_scan_offset_ = buffer.size() > 3 ? buffer.size() - 3 : 0;
        return detail::http1_client_response_parse_result_access::need_more();
    }
    // A complete head holds a line feed, so its status line was validated
    // above unless the input broke the same-prefix contract of parse().
    // A retry after an allocation failure rescans from the start.
    if (!status_line_) {
        throw std::logic_error("HTTP/1 client response input does not extend the previous prefix");
    }
    const auto status_line = *status_line_;
    const auto fields_begin = status_line_end_ + 2;
    header_scan_offset_ = 0;
    status_line_.reset();
    status_line_end_ = 0;

    // Exclude only the final empty line: every field keeps its CRLF, matching
    // the request scanner even when the head contains no fields.
    auto field_section = buffer.substr(fields_begin, header_bytes - 2 - fields_begin);
    std::optional<std::string> unfolded_fields;
    // The status line is not a field: its following whitespace cannot be
    // folded into the reason phrase. Only normalize within the field block.
    if (const auto first_fold = detail::find_field_line_fold(field_section); first_fold != std::string_view::npos) {
        unfolded_fields.emplace();
        detail::copy_unfolded_field_lines(field_section, first_fold, detail::field_line_folding_policy::http, *unfolded_fields);
        field_section = *unfolded_fields;
    }
    auto parsed_head = detail::parse_http1_client_response_head_fields(status_line, field_section, exchange_state_);
    if ((parsed_head.index() != 0)) {
        return fail(std::get<1>(parsed_head));
    }
    const auto& parsed_value = std::get<0>(parsed_head);

    auto planning = detail::plan_http1_client_response(exchange_state_, parsed_value, request_content_phase_, interim_faulty_framing_);
    if ((planning.index() != 0)) {
        return fail(std::get<1>(planning));
    }
    auto plan = std::move(std::get<0>(planning));

    // No owning response head is observable until the entire head and framing
    // plan have validated. Field views borrow the input or unfolded_head until
    // they are copied below; neither failure nor allocation commits the exchange.
    auto head = detail::http_client_response_head_access::make(parsed_value.received_status_, parsed_value.protocol_version_);
    detail::http_client_response_head_access::content_length(head, parsed_value.content_length_.value());
    const auto parsed_headers = parsed_value.headers_.view();
    if (!parsed_headers.empty()) {
        auto& headers = detail::http_client_response_head_access::headers(head);
        headers.reserve(parsed_headers.size());
        for (const auto& header_value : parsed_headers) {
            headers.emplace_back(header_value.name(), header_value.value());
        }
    }

    auto result_value = detail::http1_client_response_parse_result_access::parsed(std::move(head), std::move(plan), header_bytes);
    if (parsed_value.status_code_.is_informational() && parsed_value.protocol_version_ == http_protocol_version::http10 && parsed_value.saw_transfer_encoding_) {
        interim_faulty_framing_ = true;
    }
    if (parsed_value.status_code_ == http_status::continue_value) {
        request_content_phase_ = receive_continue(request_content_phase_);
    }
    if (parsed_value.status_code_ == http_status::switching_protocols || parsed_value.status_code_.is_final()) {
        phase_ = phase_type::complete;
    }
    return result_value;
}

void http1_client_response_parser::release_request_content() noexcept {
    if (phase_ == phase_type::await_response && request_content_phase_ == detail::http1_client_request_content_phase::awaiting_continue) {
        request_content_phase_ = detail::http1_client_request_content_phase::content_released_awaiting_continue;
    }
}

http1_client_request_content_completion_status http1_client_response_parser::complete_request_content() noexcept {
    if (phase_ != phase_type::await_response) {
        return http1_client_request_content_completion_status::exchange_terminal;
    }
    switch (request_content_phase_) {
        case detail::http1_client_request_content_phase::content_pending:
            request_content_phase_ = detail::http1_client_request_content_phase::content_complete;
            return http1_client_request_content_completion_status::completed;
        case detail::http1_client_request_content_phase::awaiting_continue:
        case detail::http1_client_request_content_phase::content_released_awaiting_continue:
            request_content_phase_ = detail::http1_client_request_content_phase::content_complete_awaiting_continue;
            return http1_client_request_content_completion_status::completed;
        case detail::http1_client_request_content_phase::continue_received:
            request_content_phase_ = detail::http1_client_request_content_phase::continue_received_content_complete;
            return http1_client_request_content_completion_status::completed;
        case detail::http1_client_request_content_phase::content_complete:
        case detail::http1_client_request_content_phase::content_complete_awaiting_continue:
        case detail::http1_client_request_content_phase::continue_received_content_complete:
            return http1_client_request_content_completion_status::already_complete;
    }
    return http1_client_request_content_completion_status::already_complete;
}

}  // namespace ruvia

namespace ruvia::detail {

namespace {

[[nodiscard]] bool request_offers_protocol(const http1_client_exchange_state& exchange_state, const http_upgrade_protocol& selected) noexcept {
    const auto offered_protocols = http1_client_exchange_state_access::offered_upgrade_protocols(exchange_state);
    if (offered_protocols.empty()) {
        return false;
    }
    bool offered = false;
    http_upgrade_protocols protocols;
    // The writer already validated this offer. Forwarded Upgrade fields may
    // retain empty members (RFC 9110 sections 2.2 and 5.6.1); selecting a
    // response protocol must not reapply generation grammar to those bytes.
    if (protocols.parse_field(offered_protocols, http_field_list_role::recipient, [&selected, &offered](const http_upgrade_protocol& candidate_value) noexcept {
        offered = offered || http_upgrade_protocol_equals(candidate_value, selected);
        return true;
    }) != http_field_list_parse_status::ok) {
        return false;
    }
    return protocols.has_protocol() && offered;
}

}  // namespace

http1_client_status_line_parse_result_type parse_http1_client_status_line(std::string_view status_line) noexcept {
    const auto separator = status_line.find(' ');
    if (separator == std::string_view::npos) {
        return http1_client_response_parse_error::invalid_status_line;
    }

    const auto version = parse_http1_version(status_line.substr(0, separator));
    if (version == http1_version_parse_result::invalid || version == http1_version_parse_result::unsupported) {
        return http1_client_response_parse_error::unsupported_http_version;
    }

    // status-line = HTTP-version SP status-code SP [ reason-phrase ]. The
    // separator after the three digits is mandatory even for an empty phrase.
    if (status_line.size() < separator + 5 || status_line[separator + 4] != ' ') {
        return http1_client_response_parse_error::invalid_status_code;
    }

    // RFC 9110 section 15: a received 000..099 or 600..999 is processed as a
    // 5xx response.
    const auto parsed_status = http_received_status::try_parse(status_line.substr(separator + 1, 3));
    if (!parsed_status) {
        return http1_client_response_parse_error::invalid_status_code;
    }

    // Unlike a field value, reason-phrase can legitimately begin or end with
    // SP/HTAB. Validate bytes directly instead of applying OWS trimming rules.
    if (!is_valid_http_field_value_bytes(status_line.substr(separator + 5))) {
        return http1_client_response_parse_error::invalid_reason_phrase;
    }

    return http1_client_parsed_status_line{.received_status_ = *parsed_status, .protocol_version_ = version == http1_version_parse_result::http10 ? http_protocol_version::http10 : http_protocol_version::http11};
}

http1_client_parsed_response_head::http1_client_parsed_response_head(const http1_client_parsed_status_line& status_line) noexcept :
    received_status_(status_line.received_status_),
    status_code_(status_line.received_status_.status()),
    protocol_version_(status_line.protocol_version_) {}

http1_client_response_head_parse_result_type parse_http1_client_response_head_fields(const http1_client_parsed_status_line& status_line, std::string_view field_section, const http1_client_exchange_state& exchange_state) {
    http1_client_parsed_response_head output(status_line);

    const auto content_semantics = classify_http_response_content_semantics(http1_client_exchange_state_access::method(exchange_state), output.status_code_);
    // RFC 9112 section 6.3 rule 1 ends 1xx, 204, 304 and HEAD responses at the
    // header section whatever framing fields they carry, and rule 1 precedes
    // the rule 5 Content-Length check. A recipient therefore ignores their
    // framing fields, even invalid ones, rather than rejecting a message it can
    // still delimit exactly; HEAD and 304 Content-Length remains a retained
    // field describing the selected representation.
    const bool framing_fields_apply = content_semantics == http_response_content_semantics::with_content;
    bool parse_length_metadata = content_semantics == http_response_content_semantics::without_content && output.status_code_ != http_status::no_content && (http1_client_exchange_state_access::method(exchange_state) == http_known_method::head || output.status_code_ == http_status::not_modified);

    std::size_t cursor_value = 0;
    while (cursor_value < field_section.size()) {
        // The field-count rule of http_field_section_budget.
        if (output.headers_.size() == max_http_header_fields) {
            return http1_client_response_parse_error::header_too_large;
        }
        const auto field = scan_http1_field_line(field_section, cursor_value);
        if (!field) {
            return http1_client_response_parse_error::invalid_header;
        }
        const auto name = field->name();
        const auto value = field->value();
        const auto kind = classify_http_field(name);
        output.headers_.append(http_header_view{name, value});

        switch (kind) {
            case http_field_kind::content_length:
                // RFC 9112 section 6.3 rule 1: 1xx (including 101), 204, 304,
                // and HEAD responses end at the first empty line regardless of
                // framing fields; successful CONNECT becomes a tunnel.
                if (framing_fields_apply || parse_length_metadata) {
                    if (const auto error = parse_http1_content_length_field<http1_client_response_parse_error>(value, output.content_length_)) {
                        if (framing_fields_apply) {
                            return *error;
                        }
                        // 无正文优先级不变；不一致的表示元数据只是不再提供已知长度。
                        output.content_length_ = {};
                        parse_length_metadata = false;
                    }
                }
                break;
            case http_field_kind::connection:
                if (output.connection_options_.parse_field(value, http_connection_field_role::response_recipient) != http_field_list_parse_status::ok) {
                    return http1_client_response_parse_error::invalid_connection;
                }
                break;
            case http_field_kind::transfer_encoding:
                output.saw_transfer_encoding_ = true;
                // Only a response with content is framed by Transfer-Encoding;
                // successful CONNECT is ignored by client-side rule.
                if (framing_fields_apply && output.protocol_version_ == http_protocol_version::http11) {
                    if (const auto error = parse_http1_transfer_encoding_field<http1_client_response_parse_error>(value, output.transfer_encoding_)) {
                        return *error;
                    }
                }
                break;
            case http_field_kind::upgrade:
                if (output.upgrade_protocols_.parse_field(value, http_field_list_role::recipient, [&exchange_state, &output, check_offered = content_semantics == http_response_content_semantics::protocol_switch](const http_upgrade_protocol& selected) noexcept {
                    if (check_offered && output.all_upgrade_protocols_offered_ && !request_offers_protocol(exchange_state, selected)) {
                        output.all_upgrade_protocols_offered_ = false;
                    }
                    return true;
                }) != http_field_list_parse_status::ok) {
                    return http1_client_response_parse_error::invalid_upgrade;
                }
                break;
            default:
                break;
        }
    }

    if (output.transfer_encoding_.unsupported()) {
        return http1_client_response_parse_error::unsupported_transfer_encoding;
    }
    // RFC 9112 section 6.1: an HTTP/1.0 message carrying Transfer-Encoding is
    // treated as if its framing were faulty and the connection closes after it.
    // The transfer codings stay unparsed; the plan ignores Transfer-Encoding and
    // falls back to Content-Length or close delimiting.
    return output;
}

}  // namespace ruvia::detail

namespace ruvia {

std::optional<std::string_view> http_client_response_head::header(std::string_view name) const& noexcept {
    return detail::find_last_http_header<http_header>(headers_, name);
}

}  // namespace ruvia

// Whether a parsed response head lets the exchange continue: may this 101 switch
// protocols given what the request offered, does the body have a length, and
// does the connection survive the response.

namespace ruvia::detail {

namespace {

[[nodiscard]] bool request_allows_protocol_switch(const http1_client_exchange_state& exchange_state, const http1_client_parsed_response_head& response, http1_client_request_content_phase request_content_phase, bool interim_faulty_framing) noexcept {
    const bool request_content_allows_switch = request_content_phase == http1_client_request_content_phase::content_complete || request_content_phase == http1_client_request_content_phase::content_complete_awaiting_continue || request_content_phase == http1_client_request_content_phase::continue_received_content_complete;
    if (interim_faulty_framing || http1_client_exchange_state_access::close_policy(exchange_state) == http1_close_policy::close_after_response || !request_content_allows_switch) {
        return false;
    }
    if (detail::http1_client_exchange_state_access::offered_upgrade_protocols(exchange_state).empty() || response.connection_options_.close() || !response.connection_options_.upgrade() || !response.upgrade_protocols_.has_protocol()) {
        return false;
    }
    return response.all_upgrade_protocols_offered_;
}

[[nodiscard]] std::optional<http_client_request_content_signal> resolve_request_content_signal(http1_client_request_content_phase phase, http_status_code status_code, bool response_will_close) noexcept {
    if (response_will_close && (phase == http1_client_request_content_phase::awaiting_continue || phase == http1_client_request_content_phase::content_pending || phase == http1_client_request_content_phase::content_released_awaiting_continue || phase == http1_client_request_content_phase::continue_received)) {
        return http_client_request_content_signal::exchange_complete;
    }
    if (status_code == http_status::continue_value) {
        return phase == http1_client_request_content_phase::awaiting_continue ? std::optional<http_client_request_content_signal>(http_client_request_content_signal::continue_value) : std::nullopt;
    }
    if (status_code.is_final() && phase == http1_client_request_content_phase::awaiting_continue) {
        // A final response cancels content only while Expect still gates it.
        // After 100 Continue or a finite wait releases the writer, RFC 9110
        // section 7.5 says to keep sending unless the server explicitly stops
        // it. Closing responses provide that signal and are handled above.
        return http_client_request_content_signal::exchange_complete;
    }
    return std::nullopt;
}

[[nodiscard]] http1_close_policy response_persistence(const http1_client_parsed_response_head& response) noexcept {
    // RFC 9112 section 6.1: an HTTP/1.0 message carrying Transfer-Encoding
    // has faulty framing and the connection closes after it, even when the
    // response ends at its header section and the field is otherwise ignored.
    if (response.connection_options_.close() || (response.protocol_version_ == http_protocol_version::http10 && response.saw_transfer_encoding_)) {
        return http1_close_policy::close_after_response;
    }
    if (response.protocol_version_ == http_protocol_version::http11 || response.connection_options_.keep_alive()) {
        return http1_close_policy::allow_reuse;
    }
    return http1_close_policy::close_after_response;
}

[[nodiscard]] http1_close_policy final_response_persistence(const http1_client_exchange_state& exchange_state, const http1_client_parsed_response_head& response, http1_client_request_content_phase request_content_phase, bool interim_faulty_framing) noexcept {
    // A final response while Expect still gates the content cancels content
    // whose framing the request head already committed. The server may keep
    // reading it (RFC 9110 section 10.1.1), so the next request on this
    // connection would be consumed as that content.
    // A rejected CONNECT is not a reusable HTTP exchange: RFC 9931 section 8
    // requires the proxy to close it before interpreting any following request.
    if (interim_faulty_framing || http1_client_exchange_state_access::method(exchange_state) == http_known_method::connect || http1_client_exchange_state_access::close_policy(exchange_state) == http1_close_policy::close_after_response || request_content_phase == http1_client_request_content_phase::awaiting_continue) {
        return http1_close_policy::close_after_response;
    }
    return response_persistence(response);
}

}  // namespace

struct http1_client_response_plan_access final {
    using request_content_signal_type = std::optional<http_client_request_content_signal>;

    [[nodiscard]] static http1_client_response_plan informational(http1_close_policy persistence, request_content_signal_type request_content_signal) noexcept {
        return http1_client_response_plan(http1_client_response_plan::state_type(http1_client_informational_response(persistence)), request_content_signal);
    }

    [[nodiscard]] static http1_client_response_plan without_content(http1_close_policy persistence, request_content_signal_type request_content_signal) noexcept {
        return http1_client_response_plan(http1_client_response_plan::state_type(http1_client_response_without_content(persistence)), request_content_signal);
    }

    [[nodiscard]] static http1_client_response_plan known_length(std::size_t content_length, http1_close_policy persistence, request_content_signal_type request_content_signal, bool discards_content) noexcept {
        return http1_client_response_plan(http1_client_response_plan::state_type(http1_client_known_length_response(content_length, persistence)), request_content_signal, discards_content);
    }

    [[nodiscard]] static http1_client_response_plan chunked(http_transfer_codings transfer_codings, http1_close_policy persistence, request_content_signal_type request_content_signal, bool discards_content) {
        return http1_client_response_plan(http1_client_response_plan::state_type(http1_client_chunked_response(std::move(transfer_codings), persistence)), request_content_signal, discards_content);
    }

    [[nodiscard]] static http1_client_response_plan close_delimited(http_transfer_codings transfer_codings, request_content_signal_type request_content_signal, bool discards_content) {
        return http1_client_response_plan(http1_client_response_plan::state_type(http1_client_close_delimited_response(std::move(transfer_codings))), request_content_signal, discards_content);
    }

    [[nodiscard]] static http1_client_response_plan connect_tunnel() noexcept {
        return http1_client_response_plan(http1_client_response_plan::state_type(http1_client_connect_tunnel()), std::nullopt);
    }

    [[nodiscard]] static http1_client_response_plan protocol_upgrade() noexcept {
        return http1_client_response_plan(http1_client_response_plan::state_type(http1_client_protocol_upgrade()), std::nullopt);
    }
};

http1_client_response_planning_result_type plan_http1_client_response(const http1_client_exchange_state& exchange_state, const http1_client_parsed_response_head& response, http1_client_request_content_phase request_content_phase, bool interim_faulty_framing) {
    const auto content_semantics = classify_http_response_content_semantics(http1_client_exchange_state_access::method(exchange_state), response.status_code_);

    if (content_semantics == http_response_content_semantics::protocol_switch) {
        if (response.protocol_version_ != http_protocol_version::http11 || !request_allows_protocol_switch(exchange_state, response, request_content_phase, interim_faulty_framing)) {
            return http1_client_response_parse_error::invalid_protocol_switch;
        }
        return http1_client_response_plan_access::protocol_upgrade();
    }
    if (content_semantics == http_response_content_semantics::informational) {
        // A 1xx never ends the exchange or cancels request content, so it cannot
        // close the connection either; the final response alone decides that
        // (RFC 9110 Section 15.2).
        return http1_client_response_plan_access::informational(http1_close_policy::allow_reuse, resolve_request_content_signal(request_content_phase, response.status_code_, false));
    }
    if (content_semantics == http_response_content_semantics::connect_tunnel) {
        return http1_client_response_plan_access::connect_tunnel();
    }

    // RFC 9110 section 15.3.6 forbids only generating 205 content. Its framing
    // is still authoritative, so delimited content is consumed and discarded
    // and the connection stays reusable.
    const bool discards_content = response.status_code_ == http_status::reset_content && content_semantics == http_response_content_semantics::with_content;
    const auto content_length = response.content_length_.value();

    const auto persistence = final_response_persistence(exchange_state, response, request_content_phase, interim_faulty_framing);
    const auto persistent_content_signal = resolve_request_content_signal(request_content_phase, response.status_code_, persistence == http1_close_policy::close_after_response);
    if (content_semantics == http_response_content_semantics::without_content) {
        return http1_client_response_plan_access::without_content(persistence, persistent_content_signal);
    }

    const auto& transfer_encoding = response.transfer_encoding_.value();
    // RFC 9112 section 6.1: HTTP/1.0 Transfer-Encoding is faulty framing, so the
    // codings are ignored and the message falls back to Content-Length or close
    // delimiting while the connection closes after it.
    const bool http10_faulty_framing = response.protocol_version_ == http_protocol_version::http10 && response.saw_transfer_encoding_;
    if (response.saw_transfer_encoding_ && !http10_faulty_framing) {
        if (content_length.has_value()) {
            return http1_client_response_parse_error::content_length_and_transfer_encoding;
        }
        if (!transfer_encoding.has_value()) {
            return http1_client_response_parse_error::invalid_transfer_encoding;
        }
        if (const auto* final_chunked = transfer_encoding->final_chunked()) {
            return http1_client_response_plan_access::chunked(final_chunked->transfer_codings(), persistence, persistent_content_signal, discards_content);
        }
        return http1_client_response_plan_access::close_delimited(transfer_encoding->non_chunked()->transfer_codings(), resolve_request_content_signal(request_content_phase, response.status_code_, true), discards_content);
    }

    if (content_length.has_value()) {
        if (*content_length > (std::numeric_limits<std::size_t>::max)()) {
            return http1_client_response_parse_error::invalid_content_length;
        }
        return http1_client_response_plan_access::known_length(static_cast<std::size_t>(*content_length), persistence, persistent_content_signal, discards_content);
    }

    // RFC 9112 section 6.3: a body-allowed response with no declared
    // length is delimited by server close and cannot return to a pool.
    return http1_client_response_plan_access::close_delimited(http_transfer_codings{}, resolve_request_content_signal(request_content_phase, response.status_code_, true), discards_content);
}

}  // namespace ruvia::detail
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
