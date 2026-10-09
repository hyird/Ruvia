#pragma once

#include <string_view>

#include "ruvia/http/detail/util/borrowed_view.h"

namespace ruvia::detail {

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

[[nodiscard]] inline bool http_strong_etag_equals(
    std::string_view left, std::string_view right) noexcept {
    return !http_is_weak_etag(left) && !http_is_weak_etag(right) && left == right;
}

[[nodiscard]] inline bool http_weak_etag_equals(
    std::string_view left, std::string_view right) noexcept {
    return http_trim_weak_etag_prefix(left) == http_trim_weak_etag_prefix(right);
}

struct http_etag_list_match_result final {
    bool valid_;
    bool matched_;
};

[[nodiscard]] inline http_etag_list_match_result http_parse_etag_list_matches(
    std::string_view values, std::string_view expected, bool strong) noexcept {
    bool matched = false;
    std::size_t offset = 0;

    while (offset < values.size()) {
        while (offset < values.size() &&
               (values[offset] == ' ' || values[offset] == '\t' || values[offset] == ',')) {
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
        matched = matched || (strong ? http_strong_etag_equals(entity_tag, expected)
                                     : http_weak_etag_equals(entity_tag, expected));

        while (offset < values.size() && (values[offset] == ' ' || values[offset] == '\t')) {
            ++offset;
        }
        if (offset < values.size() && values[offset] != ',') {
            return {false, false};
        }
    }
    return {true, matched};
}

[[nodiscard]] inline bool http_etag_list_matches(
    std::string_view values, std::string_view expected, bool strong) noexcept {
    const auto result_value = http_parse_etag_list_matches(values, expected, strong);
    return result_value.valid_ && result_value.matched_;
}

}  // namespace ruvia::detail
