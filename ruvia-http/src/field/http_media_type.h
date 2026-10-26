#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <string_view>

#include "ruvia/http/detail/field/header_token_utils.h"

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
    return is_valid_http_header_name(token);
}

struct http_media_type_parts final {
    std::string_view type_;
    std::string_view subtype_;
};

// Both values have already passed parameter syntax validation. Compare their
// decoded bytes without materializing quoted strings.
[[nodiscard]] inline bool http_media_parameter_value_equals(
    std::string_view left, std::string_view right, bool ascii_case_insensitive = false) noexcept {
    if (left.front() != '"' && right.front() != '"') {
        return ascii_case_insensitive ? http_ascii_equals_ignore_case(left, right) : left == right;
    }
    struct cursor final {
        std::string_view value_;
        http_quoted_string_cursor quoted_cursor_;
        std::size_t position_{0};

        explicit cursor(std::string_view value) noexcept
            : value_(value),
              quoted_cursor_(value) {}

        [[nodiscard]] bool next(unsigned char& out) noexcept {
            if (value_.front() == '"') {
                return quoted_cursor_.next(out);
            }
            if (position_ == value_.size()) {
                return false;
            }
            out = static_cast<unsigned char>(value_[position_++]);
            return true;
        }
    };
    cursor lhs(left);
    cursor rhs(right);
    while (true) {
        unsigned char lhs_char = 0;
        unsigned char rhs_char = 0;
        const bool has_left = lhs.next(lhs_char);
        const bool has_right = rhs.next(rhs_char);
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

// 常规参数缓存在栈上；超出的已验证后缀借用原字段，不以缓存容量限制参数数量。
// 所有视图仅在原字段保持有效且不变时有效，解析与匹配都不分配内存。
struct http_media_type_parameters final {
    struct parameter final {
        const char* name_data_;
        std::size_t name_size_;
        const char* value_data_;
        std::size_t value_size_;

        [[nodiscard]] std::string_view name() const noexcept {
            return {name_data_, name_size_};
        }

        [[nodiscard]] std::string_view value() const noexcept {
            return {value_data_, value_size_};
        }
    };
    // Only the cached prefix is initialized and read. Plain pointer/length slots
    // avoid constructing string_views for unused cache slots.
    std::array<parameter, 64> values_;
    std::size_t size_{0};
    std::string_view overflow_{};

    template <typename visitor_type>
    [[nodiscard]] bool visit(visitor_type&& visitor) const {
        for (std::size_t i = 0; i < std::min(size_, values_.size()); ++i) {
            if (!visitor(values_[i].name(), values_[i].value())) {
                return false;
            }
        }
        bool complete = true;
        http_visit_semicolon_parameters_quoted_field(overflow_,
            [&](std::string_view name, std::string_view value) {
                complete = visitor(name, value);
                return complete;
            });
        return complete;
    }

    [[nodiscard]] std::optional<std::string_view> find(std::string_view name) const noexcept {
        std::optional<std::string_view> result;
        (void)visit([&](std::string_view candidate, std::string_view value) noexcept {
            if (http_ascii_equals_ignore_case(candidate, name)) {
                result = value;
                return false;
            }
            return true;
        });
        return result;
    }

    // A quality output selects Accept semantics: q is parsed as a weight,
    // including an invalid weight's zero quality, rather than a media parameter.
    [[nodiscard]] bool parse(std::string_view value, int* quality = nullptr) noexcept {
        size_ = 0;
        overflow_ = {};
        if (quality != nullptr) {
            *quality = 1000;
        }
        return http_all_parameters(value, [&](std::string_view part) noexcept {
            if (part.empty()) {
                return true;
            }
            const auto equals = part.find('=');
            if (equals == std::string_view::npos) {
                return false;
            }
            const auto name = part.substr(0, equals);
            const auto parameter_value = part.substr(equals + 1);
            if (!http_media_token(name) || parameter_value.empty() || parameter_value != http_trim_ows(parameter_value)) {
                return false;
            }
            if (find(name).has_value()) {
                return false;
            }
            if (quality != nullptr && http_ascii_equals_ignore_case(name, "q")) {
                const auto parsed = http_parse_quality_value(parameter_value);
                *quality = parsed < 0 ? 0 : parsed;
            } else if (!is_http_token_or_quoted_string(parameter_value)) {
                return false;
            }
            if (size_ < values_.size()) {
                values_[size_] = {name.data(), name.size(), parameter_value.data(), parameter_value.size()};
            } else if (overflow_.empty()) {
                overflow_ = part;
            } else {
                overflow_ = std::string_view(overflow_.data(),
                    static_cast<std::size_t>(part.data() + part.size() - overflow_.data()));
            }
            ++size_;
            return true;
        });
    }

    [[nodiscard]] bool contains(std::string_view name, std::string_view value) const noexcept {
        const auto parameter_value = find(name);
        return parameter_value.has_value() && http_media_parameter_value_equals(*parameter_value, value,
                                                  http_ascii_equals_ignore_case(name, "charset"));
    }
};

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

[[nodiscard]] inline bool http_parse_media_type(std::string_view value, http_media_type_parts& parts,
    http_media_type_parameters& parameters) noexcept {
    return http_parse_media_type_parts(value, false, parts) && parameters.parse(value);
}

template <http_temporary_owning_char_string value_type>
bool http_parse_media_type(value_type&&, http_media_type_parts&, http_media_type_parameters&) = delete;

[[nodiscard]] inline bool is_valid_http_content_type_field_value(std::string_view value) noexcept {
    http_media_type_parts parts;
    http_media_type_parameters parameters;
    return http_parse_media_type(value, parts, parameters);
}

template <http_temporary_owning_char_string value_type>
bool is_valid_http_content_type_field_value(value_type&&) = delete;

}  // namespace ruvia::detail
