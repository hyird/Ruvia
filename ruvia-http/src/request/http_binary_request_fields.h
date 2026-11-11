#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_client.h"
#include "ruvia/http/http_expectations.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_known_method.h"
#include "ruvia/http/http_request_content_semantics.h"

#include "field/binary_field_section_size.h"
#include "field/http_connection_fields.h"
#include "field/http_field_kind.h"
#include "field/http_request_field_validation.h"
#include "parser/http_request_target.h"
#include "websocket/http_websocket_handshake_fields.h"

namespace ruvia::detail {

[[nodiscard]] inline bool http_binary_is_valid_outbound_regular_header(
    std::string_view name, std::string_view value) noexcept {
    return is_valid_http_header_name(name) && is_valid_http_header_value(value) &&
           !is_forbidden_http_binary_connection_field(name);
}

// Cross-field constraints only. Decoded :path syntax is already checked;
// senders combine this predicate with the origin-form grammar below.
[[nodiscard]] inline bool http_binary_request_path_matches_method_and_scheme(
    http_known_method method, std::string_view scheme, std::string_view path) noexcept {
    if (path.empty()) {
        return !is_http_request_scheme(scheme);
    }
    return path != "*" || method == http_known_method::options;
}

[[nodiscard]] inline bool http_binary_is_valid_regular_request_path(
    http_known_method method, std::string_view scheme, std::string_view path) noexcept {
    return http_binary_request_path_matches_method_and_scheme(method, scheme, path) &&
           (path.empty() || path == "*" || is_valid_origin_form_target(path));
}

[[nodiscard]] inline bool http_binary_is_valid_extended_connect_path(
    std::string_view scheme, std::string_view path) noexcept {
    return http_binary_request_path_matches_method_and_scheme(http_known_method::connect, scheme, path) &&
           (path.empty() || is_valid_origin_form_target(path));
}

[[nodiscard]] inline bool http_binary_is_valid_outbound_method(
    std::string_view method, http_known_method known_method) noexcept {
    if (known_method == http_known_method::connect) {
        return false;
    }
    return known_method != http_known_method::unknown || is_valid_http_method_token(method);
}

[[nodiscard]] inline bool http_binary_are_valid_outbound_request_headers(std::optional<std::string_view> authority,
    std::uint16_t default_port, std::span<const http_header_view> headers, bool allow_host,
    bool allow_trailers, binary_field_section_size& section_size, request_field_state& fields,
    http_request_field_source source, bool websocket_connect = false) noexcept {
    if (!is_valid_http_request_field_source(source)) {
        return false;
    }
    websocket_request_header_validator websocket_headers(http_field_list_role::sender);
    for (std::size_t i = 0; i < headers.size(); ++i) {
        const auto& header = headers[i];
        if (!http_binary_is_valid_outbound_regular_header(header.name(), header.value()) ||
            !section_size.add(header.name(), header.value())) {
            return false;
        }
        const auto kind = classify_http_field(header.name());
        if ((!allow_trailers && (kind == http_field_kind::trailer || kind == http_field_kind::te)) ||
            (!allow_host && kind == http_field_kind::host)) {
            return false;
        }
        // Content-Length and Expect belong exclusively to the typed request
        // content and expectation plans; the validator refuses raw fields.
        if (validate_request_field(kind, header.value(), http_field_list_role::sender, fields, source) !=
            request_field_status::ok) {
            return false;
        }
        if (kind == http_field_kind::host && authority.has_value() &&
            !authority_matches_host(*authority, header.value(), default_port)) {
            return false;
        }
        // Host is already refused for websocket extended CONNECT by the outbound field
        // check (:authority is the sole target authority there).
        if (websocket_connect &&
            (kind == http_field_kind::sec_websocket_key ||
                (kind == http_field_kind::other &&
                    http_ascii_equals_ignore_case(header.name(), "sec-websocket-accept")) ||
                !websocket_headers.observe_header(kind, header.value()))) {
            return false;
        }
    }
    return !websocket_connect || websocket_headers.client_offers_valid();
}

[[nodiscard]] inline bool http_binary_is_valid_outbound_regular_request_head(std::string_view method,
    http_known_method known_method, std::string_view scheme, std::uint16_t default_port,
    std::optional<std::string_view> authority, std::string_view path,
    std::span<const http_header_view> headers,
    http_request_content_indication content_indication, std::string_view content_length_value,
    http_client_request_expectation expectation, http_request_field_source source, std::uint64_t limit) noexcept {
    if (!http_binary_is_valid_outbound_method(method, known_method) || !is_valid_uri_scheme(scheme) ||
        (authority.has_value() && !is_valid_binary_request_authority(scheme, *authority)) ||
        !http_binary_is_valid_regular_request_path(known_method, scheme, path) ||
        (!authority.has_value() && binary_regular_request_requires_authority(scheme, path))) {
        return false;
    }
    // An asterisk-form target carries no authority information; binary senders
    // omit :authority (RFC 9113 §8.3.1 / RFC 9114 §4.3.1).
    if (path == "*" && authority.has_value()) {
        return false;
    }
    binary_field_section_size section_size(limit);
    bool expect_continue = false;
    switch (expectation) {
        case http_client_request_expectation::none:
            break;
        case http_client_request_expectation::continue_value:
            expect_continue = true;
            break;
        default:
            return false;
    }
    if (!http_client_expectation_is_valid(expect_continue, content_indication) ||
        !section_size.add(":method", method) || !section_size.add(":scheme", scheme) ||
        (authority.has_value() && !section_size.add(":authority", *authority)) ||
        !section_size.add(":path", path) ||
        (expect_continue && !section_size.add("expect", http_continue_expectation_token))) {
        return false;
    }
    request_field_state fields{request_field_framing::binary};
    if (!http_binary_are_valid_outbound_request_headers(authority, default_port, headers,
            /*allow_host=*/true,
            /*allow_trailers=*/true, section_size, fields, source)) {
        return false;
    }
    if (!content_length_value.empty() && !section_size.add("content-length", content_length_value)) {
        return false;
    }
    if (content_indication == http_request_content_indication::no_content) {
        return true;
    }
    const auto content_semantics = classify_http_request_content_semantics(method);
    // RFC 9110 section 9.3.7 applies the Content-Type requirement to generation.
    return content_semantics != http_request_content_semantics::forbidden &&
           (source == http_request_field_source::forwarded ||
               content_semantics != http_request_content_semantics::content_type_required ||
               fields.seen(http_field_kind::content_type));
}

}  // namespace ruvia::detail
