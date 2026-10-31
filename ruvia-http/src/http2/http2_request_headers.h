#pragma once

#include <string_view>

#include "ruvia/http/detail/field/http_header_section_size.h"
#include "ruvia/http/detail/field/http_trailer_fields.h"
#include "ruvia/http/detail/parser/http_parser_syntax.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_known_method.h"

#include "coding/http_content_length.h"
#include "field/http_request_field_validation.h"
#include "field/http_te_fields.h"
#include "http2/http2_header_rules.h"
#include "http2/http2_stream_state.h"
#include "parser/http_request_target.h"

namespace ruvia::detail {

struct http2_header_decode_context final {
    explicit http2_header_decode_context(http2_stream_state& stream_value) noexcept
        : stream_(stream_value) {}

    http2_stream_state& stream_;
    http_header_section_size decoded_header_list_size_;
    request_field_state request_fields_{request_field_framing::binary};
};

[[nodiscard]] inline bool http2_is_http_request_scheme(std::string_view scheme) noexcept {
    return http_ascii_equals_ignore_case(scheme, "http") || http_ascii_equals_ignore_case(scheme, "https");
}

// RFC 9110 defines both http-URI and https-URI with a mandatory authority.
// Asterisk-form OPTIONS is server-wide: the request target itself contains no
// authority information (RFC 9113 section 8.3.1). A translated HTTP/1.1
// origin-form request MUST omit :authority and carry Host instead; a direct
// HTTP/2 sender SHOULD send :authority. The target-URI authority may therefore
// be either field. When :authority is present, callers validate it with
// http2_is_valid_request_authority().
[[nodiscard]] inline bool http2_regular_request_requires_authority(
    std::string_view scheme, std::string_view path) noexcept {
    return path != "*" && http2_is_http_request_scheme(scheme);
}

// Cross-field constraints only. Decoded :path syntax is already checked;
// senders combine this predicate with the origin-form grammar below.
[[nodiscard]] inline bool http2_request_path_matches_method_and_scheme(
    http_known_method method, std::string_view scheme, std::string_view path) noexcept {
    if (path.empty()) {
        return !http2_is_http_request_scheme(scheme);
    }
    return path != "*" || method == http_known_method::options;
}

[[nodiscard]] inline bool http2_is_valid_regular_request_path(
    http_known_method method, std::string_view scheme, std::string_view path) noexcept {
    return http2_request_path_matches_method_and_scheme(method, scheme, path) &&
           (path.empty() || path == "*" || is_valid_origin_form_target(path));
}

[[nodiscard]] inline bool http2_is_valid_extended_connect_path(
    std::string_view scheme, std::string_view path) noexcept {
    return http2_request_path_matches_method_and_scheme(http_known_method::connect, scheme, path) &&
           (path.empty() || is_valid_origin_form_target(path));
}

[[nodiscard]] inline bool http2_is_valid_request_authority(
    std::string_view scheme, std::string_view authority) noexcept {
    if (http2_is_http_request_scheme(scheme)) {
        // HTTP(S) URI authority is mandatory even though an empty Host field is
        // valid HTTP/1 wire syntax for target URIs of other schemes.
        return !authority.empty() && is_valid_host_header(authority);
    }
    return is_valid_uri_authority(authority);
}

// Call only after authority admission (or validated local push submission).
// HTTP(S) already uses Host syntax; other schemes can contain URI userinfo.
[[nodiscard]] inline bool http2_request_synthesizes_host(const http2_stream_state& stream) noexcept {
    return !stream.has_host() && stream.has_authority() &&
           (http2_is_http_request_scheme(stream.request_scheme()) ||
               is_valid_host_header(stream.request_authority()));
}

[[nodiscard]] inline std::string_view http2_request_host_field(const http2_stream_state& stream) noexcept {
    if (!stream.has_host()) {
        return {};
    }
    for (std::size_t i = 0; i < stream.remote_header_count(); ++i) {
        const auto header_value = stream.remote_header_at(i);
        if (header_value.kind_ == request_header_kind::host) {
            return header_value.value_;
        }
    }
    return {};
}

// Wire :authority if present; otherwise the Host field of a translated
// origin-form request (RFC 9113 §8.3.1).
[[nodiscard]] inline std::string_view http2_effective_request_authority(
    const http2_stream_state& stream) noexcept {
    return stream.has_authority() ? stream.request_authority() : http2_request_host_field(stream);
}

[[nodiscard]] inline bool http2_has_required_request_authority(const http2_stream_state& stream) noexcept {
    if (!http2_regular_request_requires_authority(stream.request_scheme(), stream.request_path())) {
        return true;
    }
    if (stream.has_authority()) {
        return true;
    }
    return http2_is_valid_request_authority(stream.request_scheme(), http2_request_host_field(stream));
}

[[nodiscard]] inline bool http2_accumulate_header_list_bytes(
    http2_header_decode_context& context_value, std::string_view name, std::string_view value) noexcept {
    return context_value.decoded_header_list_size_.add(name, value);
}

[[nodiscard]] inline bool http2_append_cookie_header_value(
    http2_stream_state& stream, std::string_view value) {
    // RFC 9113 section 8.2.3 joins cookie-pairs with "; ". An empty crumb is
    // not a cookie-pair (RFC 6265 section 4.2.1) and would leave a leading or
    // trailing separator in the joined value, so it contributes nothing.
    if (value.empty()) {
        return true;
    }
    if (!stream.append_request_cookie_header_value(value, stream.has_cookie())) {
        return false;
    }
    stream.mark_cookie();
    return true;
}

[[nodiscard]] inline bool http2_on_decoded_initial_header(
    http2_header_decode_context& context_value, std::string_view name, std::string_view value) {
    if (!http2_accumulate_header_list_bytes(context_value, name, value)) {
        return false;
    }

    auto& stream = context_value.stream_;
    if (name.empty()) {
        return false;
    }

    if (name.front() == ':') {
        if (stream.regular_header_seen()) {
            return false;
        }
        if (name == ":method") {
            if (stream.has_method() || !is_valid_http_method_token(value)) {
                return false;
            }
            stream.assign_request_method(value);
            stream.record_request_pseudo_header(http2_request_pseudo_header::method);
            return true;
        }
        if (name == ":protocol") {
            if (stream.has_protocol() || !is_valid_http_header_name(value)) {
                return false;
            }
            stream.set_protocol(value);
            stream.record_request_pseudo_header(http2_request_pseudo_header::protocol);
            return true;
        }
        if (name == ":scheme") {
            if (stream.has_scheme() || !is_valid_uri_scheme(value)) {
                return false;
            }
            stream.assign_request_scheme(value);
            stream.mark_scheme(http_uri_scheme_default_port(value));
            stream.record_request_pseudo_header(http2_request_pseudo_header::scheme);
            return true;
        }
        if (name == ":authority") {
            if (stream.has_authority() || !is_valid_uri_authority(value)) {
                return false;
            }
            stream.assign_request_authority(value);
            stream.mark_authority();
            stream.record_request_pseudo_header(http2_request_pseudo_header::authority);
            return true;
        }
        if (name == ":path") {
            if (stream.has_path() || (!value.empty() && !is_valid_origin_or_asterisk_form_target(http_known_method::options, value))) {
                return false;
            }
            stream.assign_request_path(value);
            stream.mark_path();
            stream.record_request_pseudo_header(http2_request_pseudo_header::path);
            return true;
        }
        return false;
    }

    if (!http2_is_valid_regular_header(name, value)) {
        return false;
    }
    stream.mark_regular_header_seen();
    const auto kind = classify_request_header(name);
    if (validate_request_field(kind, value, http_field_list_role::recipient, context_value.request_fields_) !=
        request_field_status::ok) {
        return false;
    }
    switch (kind) {
        case request_header_kind::host:
            if (stream.has_authority() &&
                !authority_matches_host(stream.request_authority(), value, stream.scheme_default_port())) {
                return false;
            }
            stream.mark_host();
            break;
        case request_header_kind::cookie:
            return http2_append_cookie_header_value(stream, value);
        case request_header_kind::expect:
            // Expect is an extensible semantic list. Preserve unsupported but
            // syntactically valid members for the Web product's 417 policy.
            stream.parse_request_expectation_field(value);
            break;
        case request_header_kind::content_length: {
            http_content_length_state content_length;
            if (content_length.parse_field(value) != http_content_length_parse_status::ok ||
                !stream.declare_remote_content_length(*content_length.value())) {
                return false;
            }
            break;
        }
        default:
            break;
    }
    return stream.append_remote_header(name, value, kind);
}

[[nodiscard]] inline bool http2_on_decoded_request_trailer(
    http2_header_decode_context& context_value, std::string_view name, std::string_view value) {
    if (!http2_accumulate_header_list_bytes(context_value, name, value)) {
        return false;
    }

    // Pseudo-header and connection-specific fields, including a TE value other
    // than "trailers", are malformed (RFC 9113 sections 8.1 and 8.2.2). A field
    // merely not permitted as a trailer binds only the sender (RFC 9110 section
    // 6.5.1) and is discarded.
    if (!http2_is_valid_regular_header(name, value) ||
        (name == "te" && !is_valid_binary_http_te_field_value(value, http_field_list_role::recipient))) {
        return false;
    }
    return is_forbidden_http_request_trailer_name(name) || context_value.stream_.append_remote_trailer(name, value);
}

}  // namespace ruvia::detail
