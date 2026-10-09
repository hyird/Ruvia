#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <string_view>

#include "ruvia/http/detail/field/header_token_utils.h"
#include "ruvia/http/detail/parser/http_parser_syntax.h"

#include "field/http_quality_value.h"

// Media-type syntax (RFC 9110 section 8.3.1): splitting a field value into
// type/subtype and parameters, comparing parameter values across token and
// quoted-string forms, and validating a Content-Type field. Matching a media
// type against an Accept media-range lives in http_accept_media_type.h.

namespace ruvia::detail {

[[nodiscard]] inline std::string_view http_media_type_only(std::string_view value) noexcept {
    return http_header_token_before_parameters(value);
}

template <http_temporary_owning_char_string value_type>
std::string_view http_media_type_only(value_type&&) = delete;

[[nodiscard]] inline bool http_media_token(std::string_view token) noexcept {
    if (token.empty()) {
        return false;
    }
    return std::ranges::all_of(
        token, [](char ch) noexcept { return is_http_token_char(static_cast<unsigned char>(ch)); });
}

struct http_media_type_parts final {
    std::string_view type_;
    std::string_view subtype_;
};

// Compare media-type parameter values after removing quoted-string syntax and
// decoding quoted-pairs.  A token and its quoted equivalent therefore compare
// equal (for example, utf-8 and "utf-8") without allocating temporary strings.
// Parameter values are otherwise case-sensitive; individual media-type
// registrations define any value-specific case folding.
[[nodiscard]] inline bool http_media_parameter_value_equals(
    std::string_view left, std::string_view right, bool ascii_case_insensitive = false) noexcept {
    struct cursor final {
        std::string_view value_;
        std::size_t position_{0};
        std::size_t end_{0};
        bool quoted_{false};
        bool valid_{true};

        explicit cursor(std::string_view input) noexcept
            : value_(http_trim_ows(input)) {
            if (value_.empty()) {
                valid_ = false;
                return;
            }
            if (value_.front() == '"') {
                if (value_.size() < 2 || value_.back() != '"') {
                    valid_ = false;
                    return;
                }
                quoted_ = true;
                position_ = 1;
                end_ = value_.size() - 1;
                return;
            }
            end_ = value_.size();
            for (const auto ch : value_) {
                if (!is_http_token_char(static_cast<unsigned char>(ch))) {
                    valid_ = false;
                    return;
                }
            }
        }

        [[nodiscard]] bool next(unsigned char& out) noexcept {
            if (!valid_ || position_ >= end_) {
                return false;
            }
            auto ch = static_cast<unsigned char>(value_[position_++]);
            if (quoted_) {
                if (ch == '\\') {
                    if (position_ >= end_) {
                        valid_ = false;
                        return false;
                    }
                    ch = static_cast<unsigned char>(value_[position_++]);
                } else if (ch == '"' || !is_http_field_value_char(ch)) {
                    valid_ = false;
                    return false;
                }
                if (!is_http_field_value_char(ch)) {
                    valid_ = false;
                    return false;
                }
            }
            out = ch;
            return true;
        }
    };

    cursor lhs(left);
    cursor rhs(right);
    if (!lhs.valid_ || !rhs.valid_) {
        return false;
    }
    while (true) {
        unsigned char lhs_char = 0;
        unsigned char rhs_char = 0;
        const bool has_left = lhs.next(lhs_char);
        const bool has_right = rhs.next(rhs_char);
        if (!lhs.valid_ || !rhs.valid_) {
            return false;
        }
        if (has_left != has_right) {
            return false;
        }
        if (!has_left) {
            return true;
        }
        if (ascii_case_insensitive) {
            lhs_char = http_ascii_to_lower(lhs_char);
            rhs_char = http_ascii_to_lower(rhs_char);
        }
        if (lhs_char != rhs_char) {
            return false;
        }
    }
}

template <typename visitor_type>
[[nodiscard]] inline bool http_visit_media_type_parameters(
    std::string_view value, bool skip_quality_parameter, visitor_type&& visitor) noexcept {
    if (!http_accept_parameters_have_strict_equals(value)) {
        return false;
    }
    // Registered media types forbid duplicate parameter names. Keep a small,
    // fixed view table so validation stays allocation-free and bounded even for
    // hostile field values; an implausibly parameter-heavy item is invalidated.
    std::array<std::string_view, 64> names{};
    std::size_t name_count = 0;
    return http_all_parameters(value, [&](std::string_view part) noexcept {
        const auto equals = part.find('=');
        if (part.empty() || equals == std::string_view::npos) {
            return false;
        }
        const auto name = http_trim_ows(part.substr(0, equals));
        const auto parameter_value = http_trim_ows(part.substr(equals + 1));
        if (!http_media_token(name)) {
            return false;
        }
        for (std::size_t index = 0; index < name_count; ++index) {
            if (http_ascii_equals_ignore_case(names[index], name)) {
                return false;
            }
        }
        if (name_count == names.size()) {
            return false;
        }
        names[name_count++] = name;
        if (skip_quality_parameter && http_ascii_equals_ignore_case(name, "q")) {
            // RFC 9110 removed the old accept-ext grammar. q is the weight
            // wherever it appears, but media-range parameters after it still
            // participate in matching, so skip q itself and keep scanning.
            return true;
        }
        // Comparing a value with itself performs syntax validation as well.
        return http_media_parameter_value_equals(parameter_value, parameter_value) &&
               visitor(name, parameter_value);
    });
}

// The complete media type is already validated, with unique parameter names.
[[nodiscard]] inline bool http_offered_media_type_has_parameter(std::string_view offered,
    std::string_view expected_name, std::string_view expected_value) noexcept {
    bool found = false;
    http_visit_semicolon_parameters_quoted(offered,
        [expected_name, expected_value, &found](
            std::string_view name, std::string_view value) noexcept {
            if (!http_ascii_equals_ignore_case(name, expected_name)) {
                return true;
            }
            found = http_media_parameter_value_equals(value, expected_value, http_ascii_equals_ignore_case(name, "charset"));
            return false;
        });
    return found;
}

[[nodiscard]] inline bool http_parse_media_type_parts(
    std::string_view value, bool allow_wildcard, http_media_type_parts& parts) noexcept {
    value = http_media_type_only(value);
    const auto slash = value.find('/');
    if (slash == std::string_view::npos) {
        return false;
    }

    parts.type_ = value.substr(0, slash);
    parts.subtype_ = value.substr(slash + 1);
    const bool type_wildcard = parts.type_ == "*";
    const bool subtype_wildcard = parts.subtype_ == "*";
    if ((type_wildcard || subtype_wildcard) && !allow_wildcard) {
        return false;
    }
    if (type_wildcard && !subtype_wildcard) {
        return false;
    }
    return (type_wildcard || http_media_token(parts.type_)) &&
           (subtype_wildcard || http_media_token(parts.subtype_));
}

template <http_temporary_owning_char_string value_type>
bool http_parse_media_type_parts(value_type&&, bool, http_media_type_parts&) = delete;

[[nodiscard]] inline bool http_parse_media_type(
    std::string_view value, bool allow_wildcard, http_media_type_parts& parts) noexcept {
    return http_parse_media_type_parts(value, allow_wildcard, parts) &&
           http_visit_media_type_parameters(
               value, false, [](std::string_view, std::string_view) noexcept { return true; });
}

template <http_temporary_owning_char_string value_type>
bool http_parse_media_type(value_type&&, bool, http_media_type_parts&) = delete;

[[nodiscard]] inline bool is_valid_http_content_type_field_value(std::string_view value) noexcept {
    http_media_type_parts parts;
    return http_parse_media_type(value, false, parts);
}

template <http_temporary_owning_char_string value_type>
bool is_valid_http_content_type_field_value(value_type&&) = delete;

}  // namespace ruvia::detail
