#pragma once

#include <string_view>

#include "ruvia/http/detail/field/http_header_section_size.h"
#include "ruvia/http/detail/field/http_trailer_fields.h"
#include "ruvia/http/detail/parser/http_parser_syntax.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_known_method.h"

#include "coding/http_content_coding.h"
#include "coding/http_content_length.h"
#include "field/http_cors_fields.h"
#include "field/http_media_type.h"
#include "field/http_origin_fields.h"
#include "http2/http2_header_rules.h"
#include "http2/http2_stream_state.h"
#include "parser/http_request_target.h"
namespace ruvia::detail {

struct http2_header_decode_context final {
    explicit http2_header_decode_context(http2_stream_state& stream_value,
        http2_stream_header_decode_transaction* transaction_value = nullptr) noexcept
        : stream_(stream_value),
          transaction_(transaction_value) {}

    void commit_header_decode() noexcept {
        if (transaction_ != nullptr) {
            transaction_->commit();
        }
    }

    [[nodiscard]] bool accept_regular_field() noexcept {
        if (regular_field_count_ == max_http_header_fields) {
            return false;
        }
        ++regular_field_count_;
        return true;
    }

    http2_stream_state& stream_;
    http2_stream_header_decode_transaction* transaction_{nullptr};
    http_header_section_size decoded_header_list_size_;
    std::size_t regular_field_count_{0};
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

[[nodiscard]] inline bool http2_is_valid_regular_request_path(
    http_known_method method, std::string_view scheme, std::string_view path) noexcept {
    if (path.empty()) {
        return !http2_is_http_request_scheme(scheme);
    }
    return is_valid_origin_or_asterisk_form_target(method, path);
}

[[nodiscard]] inline bool http2_is_valid_extended_connect_path(
    std::string_view scheme, std::string_view path) noexcept {
    if (path.empty()) {
        return !http2_is_http_request_scheme(scheme);
    }
    return is_valid_origin_form_target(path);
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
            return true;
        }
        if (name == ":protocol") {
            if (stream.has_protocol() || !is_valid_http_header_name(value)) {
                return false;
            }
            stream.set_protocol(value);
            return true;
        }
        if (name == ":scheme") {
            if (stream.has_scheme() || !is_valid_uri_scheme(value)) {
                return false;
            }
            stream.assign_request_scheme(value);
            stream.mark_scheme(http_uri_scheme_default_port(value));
            return true;
        }
        if (name == ":authority") {
            if (stream.has_authority() || !is_valid_uri_authority(value)) {
                return false;
            }
            stream.assign_request_authority(value);
            stream.mark_authority();
            return true;
        }
        if (name == ":path") {
            if (stream.has_path() || (!value.empty() && !is_valid_origin_or_asterisk_form_target(value))) {
                return false;
            }
            stream.assign_request_path(value);
            stream.mark_path();
            return true;
        }
        return false;
    }

    if (!context_value.accept_regular_field() || !http2_is_valid_regular_header(name, value)) {
        return false;
    }
    stream.mark_regular_header_seen();
    const auto kind = classify_request_header(name);
    if ((kind == request_header_kind::origin && !is_valid_http_origin_field_value(value)) ||
        (kind == request_header_kind::access_control_request_method &&
            !is_valid_http_cors_request_method(value)) ||
        (kind == request_header_kind::access_control_request_headers &&
            !is_valid_http_cors_request_header_names(value))) {
        return false;
    }
    if (kind == request_header_kind::host) {
        if (stream.has_host() || !is_valid_host_header(value)) {
            return false;
        }
        if (stream.has_authority() &&
            !authority_matches_host(stream.request_authority(), value, stream.scheme_default_port())) {
            return false;
        }
        stream.mark_host();
    }
    if (kind == request_header_kind::cookie) {
        return http2_append_cookie_header_value(stream, value);
    }
    if (kind == request_header_kind::expect) {
        if (!is_valid_received_http_expect_field_value(value)) {
            return false;
        }
        // Expect is an extensible semantic list. Preserve unsupported but
        // syntactically valid members for the Web product's 417 policy.
        stream.parse_request_expectation_field(value);
    }
    if (kind == request_header_kind::content_type) {
        if (!is_valid_http_content_type_field_value(value)) {
            return false;
        }
    }
    if (kind == request_header_kind::content_encoding &&
        !is_valid_http_content_encoding_field_value(value, http_field_list_role::recipient)) {
        return false;
    }
    if (name == "trailer" &&
        !is_valid_http_request_trailer_field_value(value, http_field_list_role::recipient)) {
        return false;
    }
    if (const auto singleton_bit = singleton_request_header_bit(kind);
        singleton_bit != 0 && kind != request_header_kind::content_length) {
        if (!stream.mark_singleton_request_header(singleton_bit)) {
            return false;
        }
    }
    if (kind == request_header_kind::content_length) {
        http_content_length_state<> content_length;
        if (content_length.parse_field(value) != http_content_length_parse_status::ok) {
            return false;
        }
        if (!stream.declare_remote_content_length(*content_length.value())) {
            return false;
        }
    }
    return stream.append_remote_header(name, value, kind);
}

[[nodiscard]] inline bool http2_on_decoded_request_trailer(
    http2_header_decode_context& context_value, std::string_view name, std::string_view value) {
    if (!http2_accumulate_header_list_bytes(context_value, name, value)) {
        return false;
    }

    return context_value.accept_regular_field() && http2_is_valid_regular_header(name, value) &&
           !is_forbidden_http_request_trailer_name(name) &&
           context_value.stream_.append_remote_trailer(name, value);
}

}  // namespace ruvia::detail
