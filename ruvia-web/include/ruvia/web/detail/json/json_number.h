#pragma once

#include <charconv>
#include <cmath>
#include <cstddef>
#include <string_view>
#include <system_error>
#include <type_traits>

#include "ruvia/web/detail/json/json_lex.h"
#include "ruvia/web/detail/util/decimal_number.h"

namespace ruvia::detail {

struct json_number_token final {
    // Bytes of the RFC 8259 number at the start of the input; 0 when none.
    std::size_t length_{0};
    // Whether an integer or fraction digit is nonzero.
    bool nonzero_significand_{false};
};

[[nodiscard]] inline json_number_token scan_json_number_token(std::string_view input) noexcept {
    json_number_token token;
    std::size_t index = 0;
    const auto consume_digits = [&] {
        const auto begin = index;
        while (index < input.size() && input[index] >= '0' && input[index] <= '9') {
            token.nonzero_significand_ = token.nonzero_significand_ || input[index] != '0';
            ++index;
        }
        return index - begin;
    };
    if (index < input.size() && input[index] == '-') {
        ++index;
    }
    if (index >= input.size()) {
        return {};
    }
    if (input[index] == '0') {
        ++index;
        if (index < input.size() && input[index] >= '0' && input[index] <= '9') {
            return {};
        }
    } else if (input[index] >= '1' && input[index] <= '9') {
        (void)consume_digits();
    } else {
        return {};
    }

    if (index < input.size() && input[index] == '.') {
        ++index;
        if (consume_digits() == 0) {
            return {};
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
            return {};
        }
    }
    token.length_ = index;
    return token;
}

[[nodiscard]] inline bool skip_json_number_token(std::string_view& input) noexcept {
    const auto length = scan_json_number_token(input).length_;
    if (length == 0) {
        return false;
    }
    input.remove_prefix(length);
    return true;
}

template <typename number_t_type>
[[nodiscard]] bool parse_json_number_value(std::string_view& input, number_t_type& value) {
    skip_json_whitespace(input);
    const auto token = scan_json_number_token(input);
    if (token.length_ == 0) {
        return false;
    }

    const auto number = input.substr(0, token.length_);
    if constexpr (std::is_floating_point_v<number_t_type>) {
        // The scan already enforced the RFC 8259 grammar, which excludes the
        // non-finite spellings; only range errors remain.
        const auto parsed_value = convert_decimal_number<number_t_type>(number, token.nonzero_significand_);
        if (!parsed_value || !std::isfinite(*parsed_value)) {
            return false;
        }
        value = *parsed_value;
    } else {
        const auto [ptr, ec] = std::from_chars(number.data(), number.data() + number.size(), value);
        if (ec != std::errc{} || ptr != number.data() + number.size()) {
            return false;
        }
    }
    input.remove_prefix(token.length_);
    return true;
}

}  // namespace ruvia::detail
