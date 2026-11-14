#include "http_field_validation.h"

#include <cstdint>
#include <span>
#include <stdexcept>
#include <string_view>

#include "http_client_request_view.h"
#include "http_content_negotiation.h"
#include "http_field_kind.h"
#include "http_header.h"
#include "http_message.h"
#include "http_message_framing_fields.h"
#include "http_proxy_forwarding.h"
#include "http_request.h"
#include "http_response_stream.h"
#include "http_uri_syntax.h"

namespace ruvia::detail {
namespace {

[[nodiscard]] bool is_valid_http_cors_request_header_names(std::string_view value) noexcept {
    http_field_name_list names(value);
    bool saw_name = false;
    while (names.next()) {
        saw_name = true;
    }
    // Access-Control-Request-Headers requires at least one field name, unlike
    // the general #field-name grammar.
    return saw_name && names.valid();
}

// Host is a once-only field (RFC 9112 section 3.2) without a cached singleton
// bit; its kind index is below 32 like every singleton kind.
[[nodiscard]] constexpr std::uint32_t once_only_bit(http_field_kind kind) noexcept {
    return kind == http_field_kind::host ? 1U << static_cast<std::uint32_t>(kind) : singleton_request_header_bit(kind);
}

// RFC 9110 section 5.6.6 parameter: no whitespace around '='.
[[nodiscard]] bool is_exact_http_parameter(std::string_view part) noexcept {
    const auto equals = part.find('=');
    return equals != std::string_view::npos && is_valid_http_header_name(part.substr(0, equals)) && is_http_token_or_quoted_string(part.substr(equals + 1));
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

[[nodiscard]] bool is_valid_te(std::string_view value, http_field_list_role role, request_field_framing framing) noexcept {
    if (framing == request_field_framing::binary) {
        return is_valid_binary_http_te_field_value(value, role);
    }
    return is_valid_http_te_field_value(value, role);
}

[[nodiscard]] bool is_valid_value(http_field_kind kind, std::string_view value, http_field_list_role role, request_field_framing framing, http_request_field_source source) noexcept {
    // RFC 9110 sections 2.2 and 5.6.1: forwarding is not generation. Reuse the
    // recipient value rules only for end-to-end application metadata. Ownership
    // and singleton checks below still use the actual sender role.
    const auto application_role = source == http_request_field_source::forwarded ? http_field_list_role::recipient : role;
    switch (kind) {
        case http_field_kind::origin:
            return application_role == http_field_list_role::recipient || is_valid_http_origin_field_value(value);
        case http_field_kind::access_control_request_method:
            return application_role == http_field_list_role::recipient || is_valid_http_method_token(value);
        case http_field_kind::access_control_request_headers:
            return application_role == http_field_list_role::recipient || is_valid_http_cors_request_header_names(value);
        case http_field_kind::content_type:
            // Recipients interpret media types at the application boundary.
            return application_role == http_field_list_role::recipient || is_valid_http_content_type_field_value(value);
        case http_field_kind::content_encoding:
            // Admission leaves received metadata to the decoder. Forwarding
            // validates the recipient list grammar, not arbitrary nonempty text.
            return role == http_field_list_role::recipient || is_valid_http_content_encoding_field_value(value, application_role);
        case http_field_kind::trailer:
            // The declaration neither frames content nor authorizes actual trailers.
            return role == http_field_list_role::recipient || is_valid_http_request_trailer_field_value(value, role);
        case http_field_kind::te:
            return is_valid_te(value, role, framing);
        case http_field_kind::expect:
            // Binary field semantics feed the existing 417/ignore policy, not malformed-message rejection.
            return framing == request_field_framing::binary || is_valid_received_http_expect_field_value(value);
        case http_field_kind::host:
            return is_valid_host_header(value);
        default:
            return true;
    }
}

}  // namespace

bool request_field_state::seen(http_field_kind kind) const noexcept {
    return (seen_ & once_only_bit(kind)) != 0;
}

request_field_status validate_request_field(http_field_kind kind, std::string_view value, http_field_list_role role, request_field_state& state, http_request_field_source source) noexcept {
    if (!is_valid_http_request_field_source(source)) {
        return request_field_status::invalid;
    }
    if (role == http_field_list_role::sender && (kind == http_field_kind::expect || kind == http_field_kind::content_length)) {
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
    if (role == http_field_list_role::recipient && (kind == http_field_kind::if_modified_since || kind == http_field_kind::if_unmodified_since || kind == http_field_kind::user_agent)) {
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

bool is_protocol_managed_request_field(http_field_kind kind, std::string_view name) noexcept {
    switch (kind) {
        case http_field_kind::host:
        case http_field_kind::content_length:
        case http_field_kind::transfer_encoding:
        case http_field_kind::trailer:
        case http_field_kind::expect:
        case http_field_kind::connection:
        case http_field_kind::te:
        case http_field_kind::upgrade:
            return true;
        case http_field_kind::other:
            // Keep-Alive and Proxy-Connection have no request identity.
            return is_forbidden_http_binary_connection_field(name);
        default:
            return false;
    }
}

}  // namespace ruvia::detail

namespace ruvia {

bool is_http_protocol_managed_request_field(std::string_view name) noexcept {
    return detail::is_protocol_managed_request_field(detail::classify_http_field(name), name);
}

bool is_valid_http_request_application_field(std::string_view name, std::string_view value) noexcept {
    if (!is_valid_http_header_name(name) || !is_valid_http_header_value(value)) {
        return false;
    }
    const auto kind = detail::classify_http_field(name);
    if (detail::is_protocol_managed_request_field(kind, name)) {
        return false;
    }
    // TE 是唯一按 HTTP 版本区分的请求字段语义，已由托管字段边界排除。
    // 每次只检查一个字段；重复检测由调用方持有的字段集合负责。
    detail::request_field_state state(detail::request_field_framing::http1);
    return detail::validate_request_field(kind, value, detail::http_field_list_role::sender, state) == detail::request_field_status::ok;
}

void http_request_expectations::parse_field(std::string_view value) noexcept {
    detail::http_visit_comma_separated_quoted(value, [this](std::string_view member) noexcept {
        if (http_ascii_equals_ignore_case(member, detail::http_continue_expectation_token)) {
            flags_ |= continue_value;
        } else {
            flags_ |= unsupported;
        }
        return true;
    });
}

bool is_forbidden_http_request_trailer_field_name(std::string_view field_name) noexcept {
    return detail::is_forbidden_http_request_trailer_name(field_name);
}

bool is_forbidden_http_response_trailer_field_name(std::string_view field_name) noexcept {
    return detail::is_forbidden_response_trailer_name(field_name);
}
}  // namespace ruvia

namespace ruvia::detail {

bool is_forbidden_common_trailer_name(std::string_view name, http_field_kind kind) noexcept {
    switch (kind) {
        case http_field_kind::host:
        case http_field_kind::content_length:
        case http_field_kind::transfer_encoding:
        case http_field_kind::connection:
        case http_field_kind::content_encoding:
        case http_field_kind::content_type:
        case http_field_kind::cookie:
        case http_field_kind::expect:
        case http_field_kind::if_match:
        case http_field_kind::if_modified_since:
        case http_field_kind::if_none_match:
        case http_field_kind::if_range:
        case http_field_kind::if_unmodified_since:
        case http_field_kind::range:
        case http_field_kind::upgrade:
        case http_field_kind::authorization:
        case http_field_kind::te:
        case http_field_kind::trailer:
        case http_field_kind::proxy_authorization:
            return true;
        case http_field_kind::other:
        case http_field_kind::accept:
        case http_field_kind::accept_encoding:
        case http_field_kind::access_control_request_headers:
        case http_field_kind::access_control_request_method:
        case http_field_kind::origin:
        case http_field_kind::user_agent:
        case http_field_kind::sec_websocket_key:
        case http_field_kind::sec_websocket_protocol:
        case http_field_kind::sec_websocket_version:
        case http_field_kind::forwarded:
        case http_field_kind::x_forwarded_for:
        case http_field_kind::x_forwarded_proto:
        case http_field_kind::sec_websocket_extensions:
            break;
    }

    switch (name.size()) {
        case 10:
            return http_ascii_equals_ignore_case(name, "Keep-Alive") || http_ascii_equals_ignore_case(name, "Set-Cookie");
        case 12:
            return http_ascii_equals_ignore_case(name, "Max-Forwards");
        case 13:
            return http_ascii_equals_ignore_case(name, "Cache-Control") || http_ascii_equals_ignore_case(name, "Content-Range");
        case 16:
            return http_ascii_equals_ignore_case(name, "Proxy-Connection");
        case 18:
            return http_ascii_equals_ignore_case(name, "Proxy-Authenticate");
        default:
            return false;
    }
}

bool is_forbidden_http_request_trailer_name(std::string_view name) noexcept {
    const auto kind = classify_http_field(name);
    if (is_forbidden_common_trailer_name(name, kind)) {
        return true;
    }
    return kind == http_field_kind::access_control_request_headers || kind == http_field_kind::access_control_request_method || kind == http_field_kind::origin || http_ascii_equals_ignore_case(name, "Accept-Ranges");
}

bool is_forbidden_response_trailer_name(std::string_view name) noexcept {
    // The response header classifier is the authoritative set of standardized
    // fields Ruvia manages. RFC 9110 explicitly permits only ETag (section
    // 8.8.3) and Accept-Ranges (section 14.3) from that set in trailers; every
    // other known field lacks trailer permission or controls framing,
    // representation handling, caching, routing, cookies, methods, or CORS.
    if (const auto known = classify_response_header_name(name); known != 0 && known != response_header_etag && known != response_header_accept_ranges) {
        return true;
    }

    if (is_forbidden_common_trailer_name(name, classify_http_field(name))) {
        return true;
    }

    switch (name.size()) {
        case 3:
            // Response control data (RFC 9110 §7.4).
            return http_ascii_equals_ignore_case(name, "Age");
        case 6:
            return http_ascii_equals_ignore_case(name, "Pragma");
        case 7:
            return http_ascii_equals_ignore_case(name, "Expires") || http_ascii_equals_ignore_case(name, "Warning");
        case 11:
            return http_ascii_equals_ignore_case(name, "Retry-After");
        case 15:
            return http_ascii_equals_ignore_case(name, "X-Frame-Options") || http_ascii_equals_ignore_case(name, "Referrer-Policy") || http_ascii_equals_ignore_case(name, "Clear-Site-Data");
        case 16:
            return http_ascii_equals_ignore_case(name, "X-XSS-Protection") || http_ascii_equals_ignore_case(name, "WWW-Authenticate");
        case 18:
            return http_ascii_equals_ignore_case(name, "Permissions-Policy");
        case 19:
            return http_ascii_equals_ignore_case(name, "Content-Disposition");
        case 22:
            return http_ascii_equals_ignore_case(name, "X-Content-Type-Options");
        case 23:
            return http_ascii_equals_ignore_case(name, "Content-Security-Policy");
        case 25:
            return http_ascii_equals_ignore_case(name, "Strict-Transport-Security");
        case 35:
            return http_ascii_equals_ignore_case(name, "Content-Security-Policy-Report-Only");
        default:
            return false;
    }
}

bool is_forbidden_http_binary_response_field(std::string_view name) noexcept {
    return is_forbidden_http_binary_connection_field(name) || http_ascii_equals_ignore_case(name, "te");
}

[[nodiscard]] bool is_valid_http_response_trailer_field_value(std::string_view value, http_field_list_role role) noexcept {
    return is_valid_http_trailer_field_value(value, role, [](std::string_view name) noexcept {
        return is_forbidden_response_trailer_name(name);
    });
}

[[nodiscard]] bool response_trailer_field_valid(std::string_view name, std::string_view value) noexcept {
    return is_valid_http_header_name(name) && !is_forbidden_response_trailer_name(name) && is_valid_http_header_value(value);
}

response_field_status validate_response_field(http_field_kind kind, std::string_view value, response_field_state& state) noexcept {
    switch (kind) {
        case http_field_kind::content_type:
            if (!is_valid_http_content_type_field_value(value)) {
                return response_field_status::invalid;
            }
            if (state.content_type_seen_) {
                return response_field_status::repeated_singleton;
            }
            state.content_type_seen_ = true;
            break;
        case http_field_kind::content_encoding:
            if (!is_valid_http_content_encoding_field_value(value, http_field_list_role::sender)) {
                return response_field_status::invalid;
            }
            break;
        case http_field_kind::trailer:
            if (!is_valid_http_response_trailer_field_value(value, http_field_list_role::sender)) {
                return response_field_status::invalid;
            }
            break;
        default:
            break;
    }
    return response_field_status::ok;
}

}  // namespace ruvia::detail

namespace ruvia {

bool is_valid_http_response_field(std::string_view name, std::string_view value) noexcept {
    if (!is_valid_http_header_name(name) || !is_valid_http_header_value(value)) {
        return false;
    }
    detail::response_field_state state;
    return detail::validate_response_field(detail::classify_http_field(name), value, state) == detail::response_field_status::ok;
}

http_response_trailer_section validate_http_response_trailers(std::span<const http_header_view> trailers) {
    auto section_size = http_trailer_section_budget();
    for (const auto& trailer : trailers) {
        if (!detail::response_trailer_field_valid(trailer.name(), trailer.value()) || !section_size.add_field(trailer.name(), trailer.value())) {
            throw std::invalid_argument("invalid HTTP response trailer section");
        }
    }
    return http_response_trailer_section(trailers);
}

}  // namespace ruvia
