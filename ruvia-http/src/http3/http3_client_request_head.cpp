#include "ruvia/http/http3_client_request_head.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#include <memory_resource>
#include <string_view>
#include <variant>

#include "ruvia/http/detail/field/http_connection_fields.h"
#include "ruvia/http/detail/field/http_header_section_size.h"
#include "ruvia/http/detail/parser/http_parser_syntax.h"
#include "ruvia/http/http3_message_head.h"
#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_request_target.h"

#include "field/http_expect_token.h"
#include "field/http_request_field_validation.h"
#include "http3/http3_field_section_encoder.h"
#include "http3/http3_request_content.h"
#include "parser/http_request_target.h"

namespace ruvia {
namespace {

http3_client_request_head_failure failure(http3_client_request_head_error kind) noexcept {
    return {kind};
}
http3_client_request_head_failure field_failure(http3_field_section_error error) noexcept {
    return {http3_client_request_head_error::field_section_error, error};
}

bool authority_valid(std::string_view authority, std::string_view scheme) noexcept {
    if (http_ascii_equals_ignore_case(scheme, "http") || http_ascii_equals_ignore_case(scheme, "https")) {
        const auto host = parse_http_authority_host(authority);
        return host && !host->empty();
    }
    return !authority.empty() && detail::is_valid_uri_authority(authority);
}

}  // namespace

static std::variant<http3_client_request_head, http3_client_request_head_failure> encode_request_head(
    http3_client_request_head_view view, http3_field_section_limits limits, std::pmr::memory_resource* resource, http3_qpack_encoder* encoder, std::uint64_t stream_id) {
    auto* memory = resource != nullptr ? resource : std::pmr::get_default_resource();
    const bool connect = view.method_ == "CONNECT";
    const bool extended_connect = !view.protocol_.empty();
    const bool trace = view.method_ == "TRACE";
    if (!is_valid_http_header_name(view.method_)) {
        return failure(http3_client_request_head_error::invalid_method);
    }
    if (extended_connect && (!connect || !is_valid_http_header_name(view.protocol_))) {
        return failure(http3_client_request_head_error::invalid_protocol);
    }
    if (extended_connect && !view.peer_enable_connect_protocol_) {
        return failure(http3_client_request_head_error::connect_protocol_disabled);
    }
    if (connect && !extended_connect) {
        const auto tunnel = parse_http_authority(view.authority_);
        if (!tunnel || tunnel->port_kind() != http_authority_port_kind::value || *tunnel->port() == 0 ||
            !view.scheme_.empty() || !view.path_.empty()) {
            return failure(http3_client_request_head_error::invalid_target);
        }
        // CONNECT carries tunnel bytes, not an HTTP representation with a
        // declared Content-Length (RFC 9110, section 9.3.6).
        if (view.body_length_) {
            return failure(http3_client_request_head_error::invalid_content_length);
        }
    } else if (!detail::is_valid_uri_scheme(view.scheme_) || !authority_valid(view.authority_, view.scheme_) ||
               !((view.path_ == "*" && view.method_ == "OPTIONS") || is_valid_http_origin_form_target(view.path_))) {
        return failure(http3_client_request_head_error::invalid_target);
    }

    if (connect && view.body_length_) {
        return failure(http3_client_request_head_error::invalid_content_length);
    }
    if (trace && view.body_length_ && *view.body_length_ != 0) {
        return failure(http3_client_request_head_error::invalid_content_length);
    }

    std::size_t projected_count = (connect && !extended_connect ? 2 : 4) + (extended_connect ? 1 : 0);
    // Keep the original error precedence: overflow is checked while projecting
    // each field, but the configured byte limit follows the field-count check.
    detail::http_header_section_size section_size(std::numeric_limits<std::size_t>::max());
    if ((extended_connect && !section_size.add(":protocol", view.protocol_)) || !section_size.add(":method", view.method_) ||
        ((!connect || extended_connect) && !section_size.add(":scheme", view.scheme_)) ||
        !section_size.add(":authority", view.authority_) ||
        ((!connect || extended_connect) && !section_size.add(":path", view.path_))) {
        return field_failure(http3_field_section_error::field_list_too_large);
    }
    bool expect_continue = false;
    switch (view.expectation_) {
        case http_client_request_expectation::none:
            break;
        case http_client_request_expectation::continue_value:
            expect_continue = true;
            break;
        default:
            return failure(http3_client_request_head_error::invalid_field);
    }
    // Zero means no payload. TRACE also omits Content-Length at zero; CONNECT
    // uses the stream for tunnel bytes instead of an HTTP representation.
    const auto content_indication = !connect && (!view.body_length_ || *view.body_length_ != 0)
                                        ? http_request_content_indication::will_follow
                                        : http_request_content_indication::no_content;
    if (!http_client_expectation_is_valid(expect_continue, content_indication)) {
        return failure(http3_client_request_head_error::invalid_field);
    }
    std::size_t lowercase_bytes = 0;
    detail::request_field_state request_fields{detail::request_field_framing::binary};
    for (const auto& field : view.fields_) {
        if (!is_valid_http_header_name(field.name_) || !is_valid_http_field_value_bytes(field.value_)) {
            return failure(http3_client_request_head_error::invalid_field);
        }
        const auto kind = detail::classify_request_header(field.name_);
        // RFC 9110 section 9.3.8: never generate known credential/cookie
        // fields in TRACE. Callers must also omit application-specific secrets.
        if (trace && (kind == detail::request_header_kind::authorization ||
                         kind == detail::request_header_kind::proxy_authorization ||
                         kind == detail::request_header_kind::cookie)) {
            return failure(http3_client_request_head_error::forbidden_field);
        }
        if (detail::is_forbidden_http_binary_connection_field(field.name_)) {
            return failure(http3_client_request_head_error::forbidden_field);
        }
        switch (detail::validate_request_field(
            kind, field.value_, detail::http_field_list_role::sender, request_fields)) {
            case detail::request_field_status::ok:
                break;
            case detail::request_field_status::invalid:
                // TE beyond "trailers" is a field HTTP/3 forbids (RFC 9114
                // section 4.2); a bad Host is a target-authority error.
                return failure(kind == detail::request_header_kind::te     ? http3_client_request_head_error::forbidden_field
                               : kind == detail::request_header_kind::host ? http3_client_request_head_error::invalid_authority
                                                                           : http3_client_request_head_error::invalid_field);
            case detail::request_field_status::managed_by_sender:
                // Content-Length and Expect come from body_length_ and expectation_.
                return failure(http3_client_request_head_error::forbidden_field);
        }
        if (kind == detail::request_header_kind::host && field.value_ != view.authority_) {
            return failure(http3_client_request_head_error::invalid_authority);
        }
        const bool has_uppercase = std::any_of(field.name_.begin(), field.name_.end(),
            [](unsigned char ch) { return ch >= 'A' && ch <= 'Z'; });
        if (has_uppercase) {
            if (field.name_.size() > std::numeric_limits<std::size_t>::max() - lowercase_bytes) {
                return field_failure(http3_field_section_error::field_list_too_large);
            }
            lowercase_bytes += field.name_.size();
        }
        if (projected_count == std::numeric_limits<std::size_t>::max() ||
            !section_size.add(field.name_, field.value_)) {
            return field_failure(http3_field_section_error::field_list_too_large);
        }
        ++projected_count;
        if (projected_count > limits.max_fields_) {
            return field_failure(http3_field_section_error::too_many_fields);
        }
        if (section_size.bytes() > limits.max_decoded_bytes_) {
            return field_failure(http3_field_section_error::field_list_too_large);
        }
    }
    const bool explicit_content = !connect &&
                                  (!view.body_length_ || *view.body_length_ != 0 || (view.emit_content_length_ && !trace));
    if (explicit_content && !detail::http3_request_payload_allowed(view.method_, request_fields.seen(detail::request_header_kind::content_type))) {
        return failure(trace ? http3_client_request_head_error::invalid_content_length
                             : http3_client_request_head_error::invalid_field);
    }
    if (expect_continue) {
        if (projected_count == std::numeric_limits<std::size_t>::max() ||
            !section_size.add("expect", detail::http_continue_expectation_token)) {
            return field_failure(http3_field_section_error::field_list_too_large);
        }
        ++projected_count;
    }
    const bool emit_length = view.emit_content_length_ && view.body_length_ && !trace;
    std::array<char, 20> length_bytes{};
    std::size_t length_size = 0;
    if (emit_length) {
        auto [end, ec] = std::to_chars(length_bytes.data(), length_bytes.data() + length_bytes.size(), *view.body_length_);
        if (ec != std::errc{}) {
            return failure(http3_client_request_head_error::invalid_content_length);
        }
        length_size = static_cast<std::size_t>(end - length_bytes.data());
        if (projected_count == std::numeric_limits<std::size_t>::max() ||
            !section_size.add("content-length", {length_bytes.data(), length_size})) {
            return field_failure(http3_field_section_error::field_list_too_large);
        }
        ++projected_count;
    }
    if (projected_count > limits.max_fields_) {
        return field_failure(http3_field_section_error::too_many_fields);
    }
    if (section_size.bytes() > limits.max_decoded_bytes_) {
        return field_failure(http3_field_section_error::field_list_too_large);
    }

    std::pmr::vector<char> lowercase(memory);
    lowercase.reserve(lowercase_bytes);
    std::pmr::vector<http3_field_section_field_view> fields(memory);
    fields.reserve(projected_count);
    fields.push_back({":method", view.method_, false});
    if (extended_connect) {
        fields.push_back({":protocol", view.protocol_, false});
    }
    if (!connect || extended_connect) {
        fields.push_back({":scheme", view.scheme_, false});
        fields.push_back({":authority", view.authority_, false});
        fields.push_back({":path", view.path_, false});
    } else {
        fields.push_back({":authority", view.authority_, false});
    }
    for (const auto& field : view.fields_) {
        std::string_view name = field.name_;
        const bool has_uppercase = std::any_of(name.begin(), name.end(),
            [](unsigned char ch) { return ch >= 'A' && ch <= 'Z'; });
        if (has_uppercase) {
            const auto offset = lowercase.size();
            for (const unsigned char ch : name) {
                lowercase.push_back(static_cast<char>(http_ascii_to_lower(ch)));
            }
            name = std::string_view(lowercase.data() + offset, field.name_.size());
        }
        fields.push_back({name, field.value_, false});
    }
    if (expect_continue) {
        fields.push_back({"expect", detail::http_continue_expectation_token, false});
    }
    if (emit_length) {
        fields.push_back({"content-length", {length_bytes.data(), length_size}, false});
    }
    auto encoded = detail::http3_headers_frame_access::encode(fields, memory, limits, encoder, stream_id, section_size.bytes());
    if ((encoded.index() != 0)) {
        return field_failure(std::get<1>(encoded));
    }

    http3_client_request_head result{std::move(std::get<0>(encoded))};
    result.body_plan_.expected_length_ = trace ? std::optional<std::uint64_t>{0} : view.body_length_;
    return result;
}

std::variant<http3_client_request_head, http3_client_request_head_failure> encode_http3_client_request_head(
    http3_client_request_head_view view, http3_field_section_limits limits, std::pmr::memory_resource* resource) {
    return encode_request_head(view, limits, resource, nullptr, 0);
}
std::variant<http3_client_request_head, http3_client_request_head_failure> encode_http3_client_request_head(
    http3_qpack_encoder& encoder, std::uint64_t stream_id, http3_client_request_head_view view,
    http3_field_section_limits limits, std::pmr::memory_resource* resource) {
    return encode_request_head(view, limits, resource, &encoder, stream_id);
}

}  // namespace ruvia
