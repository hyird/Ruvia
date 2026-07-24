#pragma once

#include <charconv>
#include <cmath>
#include <cstddef>
#include <string_view>
#include <system_error>
#include <type_traits>

#include "ruvia/web/detail/util/DecimalNumber.h"
#include "ruvia/web/detail/json/JsonLex.h"

namespace ruvia::detail {

[[nodiscard]] inline std::size_t scanJsonNumberTokenLength(std::string_view input) noexcept {
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
        const auto fractionBegin = index;
        while (index < input.size() && input[index] >= '0' && input[index] <= '9') {
            ++index;
        }
        if (index == fractionBegin) {
            return 0;
        }
    }
    if (index < input.size() && (input[index] == 'e' || input[index] == 'E')) {
        ++index;
        if (index < input.size() && (input[index] == '+' || input[index] == '-')) {
            ++index;
        }
        const auto exponentBegin = index;
        while (index < input.size() && input[index] >= '0' && input[index] <= '9') {
            ++index;
        }
        if (index == exponentBegin) {
            return 0;
        }
    }
    return index;
}

[[nodiscard]] inline bool skipJsonNumberToken(std::string_view& input) noexcept {
    const auto length = scanJsonNumberTokenLength(input);
    if (length == 0) {
        return false;
    }
    input.remove_prefix(length);
    return true;
}

template <typename NumberT>
[[nodiscard]] bool parseJsonNumberValue(std::string_view& input, NumberT& value) {
    skipJsonWhitespace(input);
    const auto length = scanJsonNumberTokenLength(input);
    if (length == 0) {
        return false;
    }

    const auto number = input.substr(0, length);
    if constexpr (std::is_floating_point_v<NumberT>) {
        double parsed = 0;
        if (!parseDecimalNumber(number, parsed)) {
            return false;
        }
        value = static_cast<NumberT>(parsed);
        if (!std::isfinite(value)) {
            return false;
        }
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
