#include "field/http_request_field_validation.h"

#include "ruvia/http/detail/field/http_field_list_grammar.h"
#include "ruvia/http/detail/field/http_trailer_fields.h"
#include "ruvia/http/http_field_syntax.h"
#include "ruvia/http/http_known_method.h"

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

// RFC 9110 section 5.6.6 parameter: no whitespace around '='.
[[nodiscard]] bool is_exact_http_parameter(std::string_view part) noexcept {
    const auto equals = part.find('=');
    return equals != std::string_view::npos && is_valid_http_header_name(part.substr(0, equals)) &&
           is_http_token_or_quoted_string(part.substr(equals + 1));
}

[[nodiscard]] bool http_expectation_item(std::string_view item) noexcept {
    item = http_trim_ows(item);
    if (item.empty()) {
        return false;
    }

    const auto parameters_start = http_find_unquoted_delimiter(item, 0, ';');
    const auto leading = http_trim_ows(item.substr(0, parameters_start));
    if (leading.find('=') == std::string_view::npos) {
        return parameters_start == item.size() && is_valid_http_header_name(leading);
    }
    // RFC 9110 section 5.6.6: parameters = *( OWS ";" OWS [ parameter ] ).
    return is_exact_http_parameter(leading) && http_all_parameters(item, [](std::string_view parameter) noexcept {
        return parameter.empty() || is_exact_http_parameter(parameter);
    });
}

// Recipient-side Expect grammar. Empty list members are ignored, but every
// non-empty member still has to be a syntactically valid expectation before
// product policy decides whether unsupported extensions are rejected with 417
// or ignored.
[[nodiscard]] bool is_valid_received_http_expect_field_value(std::string_view value) noexcept {
    return http_visit_list_members(value, http_field_list_role::recipient, [](std::string_view item) noexcept {
        return http_expectation_item(item) ? http_field_list_parse_status::ok : http_field_list_parse_status::malformed;
    }) == http_field_list_parse_status::ok;
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
    http_field_list_role role, request_field_framing framing, http_request_field_source source) noexcept {
    // RFC 9110 sections 2.2 and 5.6.1: forwarding is not generation. Reuse the
    // recipient value rules only for end-to-end application metadata. Ownership
    // and singleton checks below still use the actual sender role.
    const auto application_role = source == http_request_field_source::forwarded
                                      ? http_field_list_role::recipient
                                      : role;
    switch (kind) {
        case request_header_kind::origin:
            return application_role == http_field_list_role::recipient || is_valid_http_origin_field_value(value);
        case request_header_kind::access_control_request_method:
            return application_role == http_field_list_role::recipient || is_valid_http_method_token(value);
        case request_header_kind::access_control_request_headers:
            return application_role == http_field_list_role::recipient || is_valid_http_cors_request_header_names(value);
        case request_header_kind::content_type:
            // Recipients interpret media types at the application boundary.
            return application_role == http_field_list_role::recipient ||
                   is_valid_http_content_type_field_value(value);
        case request_header_kind::content_encoding:
            // Admission leaves received metadata to the decoder. Forwarding
            // validates the recipient list grammar, not arbitrary nonempty text.
            return role == http_field_list_role::recipient ||
                   is_valid_http_content_encoding_field_value(value, application_role);
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
    http_field_list_role role, request_field_state& state, http_request_field_source source) noexcept {
    if (!is_valid_http_request_field_source(source)) {
        return request_field_status::invalid;
    }
    if (role == http_field_list_role::sender &&
        (kind == request_header_kind::expect || kind == request_header_kind::content_length)) {
        return request_field_status::managed_by_sender;
    }
    if (!is_valid_value(kind, value, role, state.framing_, source)) {
        return request_field_status::invalid;
    }
    // Invalid date conditions, including repeated field lines, are ignored
    // rather than rejecting the request (RFC 9110 sections 13.1.3 and 13.1.4).
    // A repeated User-Agent violates only the sender rule of RFC 9110 section
    // 5.3 and controls no framing, routing or access, so recipients keep its
    // lines. Senders still cannot generate these singleton fields more than once.
    if (role == http_field_list_role::recipient &&
        (kind == request_header_kind::if_modified_since ||
            kind == request_header_kind::if_unmodified_since || kind == request_header_kind::user_agent)) {
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
