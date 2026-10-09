#pragma once

#include <charconv>
#include <cmath>
#include <cstddef>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <variant>

#include "ruvia/core/decimal_number.h"
#include "ruvia/web/detail/json/json_lex.h"

namespace ruvia::detail {

[[nodiscard]] inline std::size_t scan_json_number_token_length(std::string_view input) noexcept {
    std::size_t index = 0;
    if (index < input.size() && input[index] == '-') {
        ++index;
    }
    if (index >= input.size()) {
        return 0;
    }
    if (input[index] == '0') {
        ++index;
        if (index < input.size() && input[index] >= '0' && input[index] <= '9') {
            return 0;
        }
    } else if (input[index] >= '1' && input[index] <= '9') {
        do {
            ++index;
        } while (index < input.size() && input[index] >= '0' && input[index] <= '9');
    } else {
        return 0;
    }

    if (index < input.size() && input[index] == '.') {
        ++index;
        const auto fraction_begin = index;
        while (index < input.size() && input[index] >= '0' && input[index] <= '9') {
            ++index;
        }
        if (index == fraction_begin) {
            return 0;
        }
    }
    if (index < input.size() && (input[index] == 'e' || input[index] == 'E')) {
        ++index;
        if (index < input.size() && (input[index] == '+' || input[index] == '-')) {
            ++index;
        }
        const auto exponent_begin = index;
        while (index < input.size() && input[index] >= '0' && input[index] <= '9') {
            ++index;
        }
        if (index == exponent_begin) {
            return 0;
        }
    }
    return index;
}

[[nodiscard]] inline bool skip_json_number_token(std::string_view& input) noexcept {
    const auto length = scan_json_number_token_length(input);
    if (length == 0) {
        return false;
    }
    input.remove_prefix(length);
    return true;
}

template <typename number_t_type>
[[nodiscard]] bool parse_json_number_value(std::string_view& input, number_t_type& value) {
    skip_json_whitespace(input);
    const auto length = scan_json_number_token_length(input);
    if (length == 0) {
        return false;
    }

    const auto number = input.substr(0, length);
    if constexpr (std::is_floating_point_v<number_t_type>) {
        const auto parsed_value = ruvia::parse_decimal_number<number_t_type>(number);
        if ((parsed_value.index() != 0) || !std::isfinite(std::get<0>(parsed_value))) {
            return false;
        }
        value = std::get<0>(parsed_value);
    } else {
        const auto [ptr, ec] = std::from_chars(number.data(), number.data() + number.size(), value);
        if (ec != std::errc{} || ptr != number.data() + number.size()) {
            return false;
        }
    }
    input.remove_prefix(length);
    return true;
}

}  // namespace ruvia::detail
