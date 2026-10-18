#include "field/http_request_field_validation.h"

#include "ruvia/http/detail/field/header_token_utils.h"
#include "ruvia/http/detail/field/http_trailer_fields.h"
#include "ruvia/http/detail/parser/http_parser_syntax.h"
#include "ruvia/http/http_field_whitespace.h"

#include "coding/http_content_coding.h"
#include "field/http_cors_fields.h"
#include "field/http_media_type.h"
#include "field/http_origin_fields.h"
#include "field/http_te_fields.h"
#include "parser/http_request_target.h"

namespace ruvia::detail {
namespace {

// Host is a once-only field (RFC 9112 section 3.2) without a cached singleton
// bit; its kind index is below 32 like every singleton kind.
[[nodiscard]] constexpr std::uint32_t once_only_bit(request_header_kind kind) noexcept {
    return kind == request_header_kind::host ? 1U << static_cast<std::uint32_t>(kind)
                                             : singleton_request_header_bit(kind);
}

[[nodiscard]] bool http_expectation_token(std::string_view token) noexcept {
    if (token.empty()) {
        return false;
    }
    for (const auto ch : token) {
        if (!is_http_token_char(static_cast<unsigned char>(ch))) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool http_expectation_value(std::string_view value) noexcept {
    return http_expectation_token(value) || http_is_quoted_string(value);
}

[[nodiscard]] bool http_expectation_item(std::string_view item) noexcept {
    item = http_trim_ows(item);
    if (item.empty()) {
        return false;
    }

    const auto parameters_start = http_find_unquoted_delimiter(item, 0, ';');
    const auto leading = http_trim_ows(item.substr(0, parameters_start));
    const auto equals = leading.find('=');
    if (equals == std::string_view::npos) {
        return parameters_start == item.size() && http_expectation_token(leading);
    }

    const auto name = leading.substr(0, equals);
    const auto value = leading.substr(equals + 1);
    if (!http_expectation_token(name) || !http_expectation_value(value)) {
        return false;
    }

    std::size_t start = parameters_start;
    while (start < item.size()) {
        ++start;
        const auto end = start + http_find_unquoted_delimiter(item.substr(start), 0, ';');
        const auto parameter = http_trim_ows(item.substr(start, end - start));
        // RFC 9110 section 5.6.6: parameters = *( OWS ";" OWS [ parameter ] ).
        if (parameter.empty()) {
            start = end;
            continue;
        }
        const auto parameter_equals = parameter.find('=');
        if (parameter_equals == std::string_view::npos) {
            return false;
        }
        const auto parameter_name = parameter.substr(0, parameter_equals);
        const auto parameter_value = parameter.substr(parameter_equals + 1);
        if (!http_expectation_token(parameter_name) || !http_expectation_value(parameter_value)) {
            return false;
        }
        start = end;
    }
    return true;
}

// Recipient-side Expect grammar. Empty list members are tolerated by the generic
// field-list parser, but every non-empty member still has to be a syntactically
// valid expectation before product policy decides whether unsupported extensions
// are rejected with 417 or ignored.
[[nodiscard]] bool is_valid_received_http_expect_field_value(std::string_view value) noexcept {
    bool valid = true;
    http_visit_comma_separated_quoted_field_items(value, [&valid](std::string_view item) noexcept {
        item = http_trim_ows(item);
        if (item.empty()) {
            return true;
        }
        if (!http_expectation_item(item)) {
            valid = false;
            return false;
        }
        return true;
    });
    return valid;
}

[[nodiscard]] bool is_valid_te(
    std::string_view value, http_field_list_role role, request_field_framing framing) noexcept {
    if (framing == request_field_framing::binary) {
        return is_valid_binary_http_te_field_value(value, role);
    }
    return role == http_field_list_role::sender ? is_valid_client_http_te_field_value(value)
                                                : is_valid_received_http_te_field_value(value);
}

[[nodiscard]] bool is_valid_value(request_header_kind kind, std::string_view value,
    http_field_list_role role, request_field_framing framing) noexcept {
    switch (kind) {
        case request_header_kind::origin:
            return role == http_field_list_role::recipient || is_valid_http_origin_field_value(value);
        case request_header_kind::access_control_request_method:
            return role == http_field_list_role::recipient || is_valid_http_cors_request_method(value);
        case request_header_kind::access_control_request_headers:
            return role == http_field_list_role::recipient || is_valid_http_cors_request_header_names(value);
        case request_header_kind::content_type:
            // Recipients interpret media types at the application boundary.
            return role == http_field_list_role::recipient ||
                   is_valid_http_content_type_field_value(value);
        case request_header_kind::content_encoding:
            return is_valid_http_content_encoding_field_value(value, role);
        case request_header_kind::trailer:
            // The declaration neither frames content nor authorizes actual trailers.
            return role == http_field_list_role::recipient ||
                   is_valid_http_request_trailer_field_value(value, role);
        case request_header_kind::te:
            return is_valid_te(value, role, framing);
        case request_header_kind::expect:
            // Binary field semantics feed the existing 417/ignore policy, not malformed-message rejection.
            return framing == request_field_framing::binary ||
                   is_valid_received_http_expect_field_value(value);
        case request_header_kind::host:
            return is_valid_host_header(value);
        default:
            return true;
    }
}

}  // namespace

bool request_field_state::seen(request_header_kind kind) const noexcept {
    return (seen_ & once_only_bit(kind)) != 0;
}

request_field_status validate_request_field(request_header_kind kind, std::string_view value,
    http_field_list_role role, request_field_state& state) noexcept {
    if (role == http_field_list_role::sender &&
        (kind == request_header_kind::expect || kind == request_header_kind::content_length)) {
        return request_field_status::managed_by_sender;
    }
    if (!is_valid_value(kind, value, role, state.framing_)) {
        return request_field_status::invalid;
    }
    // Invalid date conditions, including repeated field lines, are ignored
    // rather than rejecting the request (RFC 9110 sections 13.1.3 and 13.1.4).
    // Senders still cannot generate these singleton fields more than once.
    if (role == http_field_list_role::recipient &&
        (kind == request_header_kind::if_modified_since ||
            kind == request_header_kind::if_unmodified_since)) {
        return request_field_status::ok;
    }
    if (const auto bit = once_only_bit(kind); bit != 0) {
        if ((state.seen_ & bit) != 0) {
            return request_field_status::invalid;
        }
        state.seen_ |= bit;
    }
    return request_field_status::ok;
}

}  // namespace ruvia::detail
