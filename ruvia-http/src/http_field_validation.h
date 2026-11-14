#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "borrowed_text.h"
#include "http_authority.h"
#include "http_client_request_view.h"
#include "http_field_kind.h"
#include "http_header.h"

namespace ruvia::detail {

// Shared quality parsing (RFC 9110 section 12.4.2). Token fields use one
// optional weight; media ranges extract q from their parameters.

[[nodiscard]] inline int http_parse_quality_value(std::string_view value) noexcept {
    value = http_trim_ows(value);
    if (value == "1") {
        return 1000;
    }
    if (value == "0") {
        return 0;
    }
    if (value.size() >= 2 && value[1] == '.' && (value[0] == '0' || value[0] == '1')) {
        int quality = value[0] == '1' ? 1000 : 0;
        if (value[0] == '1') {
            for (std::size_t i = 2; i < value.size(); ++i) {
                if (i > 4 || value[i] != '0') {
                    return -1;
                }
            }
            return quality;
        }

        int scale = 100;
        for (std::size_t i = 2; i < value.size(); ++i) {
            if (i > 4 || value[i] < '0' || value[i] > '9') {
                return -1;
            }
            quality += (value[i] - '0') * scale;
            scale /= 10;
        }
        return quality;
    }
    return -1;
}

// Token-based Accept fields allow one optional weight, not media parameters.
// The list visitor has already trimmed surrounding OWS. Invalid weights are
// unacceptable rather than inheriting the unweighted default quality.
[[nodiscard]] inline int http_weight_parameter(std::string_view value) noexcept {
    const auto semicolon = value.find(';');
    if (semicolon == std::string_view::npos) {
        return 1000;
    }
    auto weight = http_trim_ows(value.substr(semicolon + 1));
    if (weight.size() < 3 || http_ascii_to_lower(static_cast<unsigned char>(weight[0])) != 'q' || weight[1] != '=') {
        return 0;
    }
    const auto qvalue = weight.substr(2);
    if (qvalue != http_trim_ows(qvalue)) {
        return 0;
    }
    const auto parsed_value = http_parse_quality_value(qvalue);
    return parsed_value < 0 ? 0 : parsed_value;
}

enum class response_field_status : std::uint8_t {
    ok,
    invalid,
    repeated_singleton,
};

// One state per generated response header section. Content-Type's single media
// type cannot be combined into a list (RFC 9110 section 5.3).
class response_field_state final {
private:
    friend response_field_status validate_response_field(http_field_kind kind, std::string_view value, response_field_state& state) noexcept;

    bool content_type_seen_{false};
};

// Sender-only response metadata semantics. Callers validate field-name/value bytes
// and retain protocol-specific framing, connection, and forbidden-field rules.
// A fresh state also validates one value before an http_response setter commits
// its replacement; the setter owns the separate append/replace policy.
[[nodiscard]] response_field_status validate_response_field(http_field_kind kind, std::string_view value, response_field_state& state) noexcept;

[[nodiscard]] bool is_forbidden_response_trailer_name(std::string_view name) noexcept;

enum class http_interim_response_header_validation_status : std::uint8_t {
    ok,
    invalid_header,
    content_length_forbidden,
    transfer_encoding_forbidden,
    trailer_forbidden,
    repeated_singleton,
};

// Incremental sender validation for a version-neutral interim head. Recipients
// use their version's field syntax and message-boundary rules instead: 1xx
// framing advertisements do not establish content (RFC 9112 section 6.3 and
// RFC 9113 section 8.1.1).
class http_interim_response_sender_validator final {
public:
    // kind_out receives the field identity of an accepted line.
    [[nodiscard]] http_interim_response_header_validation_status validate(std::string_view name, std::string_view value, http_field_kind* kind_out = nullptr) noexcept {
        if (!is_valid_http_header_name(name) || !is_valid_http_header_value(value)) {
            return http_interim_response_header_validation_status::invalid_header;
        }

        const auto field = classify_http_field_name(name);
        const auto known_bit = field.response_bit_;
        const auto kind = field.kind_;
        switch (kind) {
            case http_field_kind::content_length:
                return http_interim_response_header_validation_status::content_length_forbidden;
            case http_field_kind::transfer_encoding:
                return http_interim_response_header_validation_status::transfer_encoding_forbidden;
            case http_field_kind::trailer:
                return http_interim_response_header_validation_status::trailer_forbidden;
            default:
                break;
        }
        switch (validate_response_field(kind, value, fields_)) {
            case response_field_status::ok:
                break;
            case response_field_status::invalid:
                return http_interim_response_header_validation_status::invalid_header;
            case response_field_status::repeated_singleton:
                return http_interim_response_header_validation_status::repeated_singleton;
        }
        if (known_bit != 0 && known_bit != response_header_content_type && (known_bits_ & known_bit) != 0 && response_header_append_forbidden(known_bit)) {
            return http_interim_response_header_validation_status::repeated_singleton;
        }
        known_bits_ |= known_bit;
        if (kind_out != nullptr) {
            *kind_out = kind;
        }
        return http_interim_response_header_validation_status::ok;
    }

private:
    response_field_state fields_;
    std::uint32_t known_bits_{0};
};

[[nodiscard]] inline std::string_view http_trim_weak_etag_prefix(std::string_view value) noexcept {
    if (value.size() >= 2 && value[0] == 'W' && value[1] == '/') {
        value.remove_prefix(2);
    }
    return value;
}

template <http_temporary_owning_char_string value_type>
std::string_view http_trim_weak_etag_prefix(value_type&&) = delete;

[[nodiscard]] inline bool http_is_weak_etag(std::string_view value) noexcept {
    return value.size() >= 2 && value[0] == 'W' && value[1] == '/';
}

[[nodiscard]] inline bool http_is_strong_etag(std::string_view value) noexcept {
    return !value.empty() && value.front() == '"';
}

[[nodiscard]] inline bool http_strong_etag_equals(std::string_view left, std::string_view right) noexcept {
    return !http_is_weak_etag(left) && !http_is_weak_etag(right) && left == right;
}

[[nodiscard]] inline bool http_weak_etag_equals(std::string_view left, std::string_view right) noexcept {
    return http_trim_weak_etag_prefix(left) == http_trim_weak_etag_prefix(right);
}

struct http_etag_list_match_result final {
    bool valid_;
    bool matched_;
};

[[nodiscard]] inline http_etag_list_match_result http_parse_etag_list_matches(std::string_view values, std::string_view expected, bool strong) noexcept {
    bool matched = false;
    std::size_t offset = 0;

    while (offset < values.size()) {
        while (offset < values.size() && (is_http_ows_char(values[offset]) || values[offset] == ',')) {
            ++offset;
        }
        if (offset == values.size()) {
            break;
        }

        const std::size_t begin = offset;
        if (values.substr(offset).starts_with("W/")) {
            offset += 2;
        }
        if (offset == values.size() || values[offset] != '"') {
            return {false, false};
        }
        ++offset;
        while (offset < values.size() && values[offset] != '"') {
            const auto byte = static_cast<unsigned char>(values[offset]);
            if (byte != 0x21 && !(byte >= 0x23 && byte <= 0x7e) && byte < 0x80) {
                return {false, false};
            }
            ++offset;
        }
        if (offset == values.size()) {
            return {false, false};
        }
        ++offset;
        const auto entity_tag = values.substr(begin, offset - begin);
        matched = matched || (strong ? http_strong_etag_equals(entity_tag, expected) : http_weak_etag_equals(entity_tag, expected));

        while (offset < values.size() && is_http_ows_char(values[offset])) {
            ++offset;
        }
        if (offset < values.size() && values[offset] != ',') {
            return {false, false};
        }
    }
    return {true, matched};
}

// RFC 6454 section 7.1 permits either `null` or a space-delimited list of
// serialized origins. Fetch-generated CORS requests currently send one item,
// but the HTTP protocol primitive must retain the complete field grammar.
[[nodiscard]] inline bool is_valid_http_origin_field_value(std::string_view value) noexcept {
    value = http_trim_ows(value);
    if (value == "null") {
        return true;
    }
    std::size_t offset = 0;
    for (;;) {
        const auto separator = value.find(' ', offset);
        const auto end = separator == std::string_view::npos ? value.size() : separator;
        if (!is_valid_http_serialized_origin(value.substr(offset, end - offset))) {
            return false;
        }
        if (separator == std::string_view::npos) {
            return true;
        }
        offset = separator + 1;
    }
}

[[nodiscard]] constexpr bool is_valid_http_request_field_source(http_request_field_source source) noexcept {
    return source == http_request_field_source::generated || source == http_request_field_source::forwarded;
}

// The only version-dependent request-field semantics: HTTP/1.1 negotiates
// transfer codings through TE, while HTTP/2 permits only "trailers".
enum class request_field_framing : std::uint8_t { http1,
    binary };

enum class request_field_status : std::uint8_t {
    ok,
    // Grammar violation, or a second instance of a field that may occur once.
    invalid,
    // A client generated the field itself. Expect and Content-Length are
    // derived from the request plan on every HTTP version, so a raw field
    // would be a second source of truth for the body gate or the framing.
    managed_by_sender,
};

// Per field section: which once-only fields were seen, plus the framing whose
// TE rules apply. One instance spans exactly one request header section.
class request_field_state final {
public:
    explicit request_field_state(request_field_framing framing) noexcept :
        framing_(framing) {}

    // True once a field that may occur only once (Host and every
    // singleton_request_header_bit() kind) has been validated.
    [[nodiscard]] bool seen(http_field_kind kind) const noexcept;

private:
    friend request_field_status validate_request_field(http_field_kind kind, std::string_view value, http_field_list_role role, request_field_state& state, http_request_field_source source) noexcept;

    std::uint32_t seen_{0};
    request_field_framing framing_;
};

// The authoritative semantic check for one request field, shared by the
// HTTP/1 and HTTP/2 request writers (role sender) and request
// recipients (role recipient). Callers classify the name once with
// classify_http_field() and keep version-specific concerns themselves:
// framing state (Content-Length values on receipt, Transfer-Encoding,
// Connection, Upgrade), Host/:authority agreement, cookie joining, and parsing
// accepted Expect members.
[[nodiscard]] request_field_status validate_request_field(http_field_kind kind, std::string_view value, http_field_list_role role, request_field_state& state, http_request_field_source source = http_request_field_source::generated) noexcept;

[[nodiscard]] bool is_protocol_managed_request_field(http_field_kind kind, std::string_view name) noexcept;

}  // namespace ruvia::detail
