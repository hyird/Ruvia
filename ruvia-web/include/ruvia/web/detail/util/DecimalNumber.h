#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

namespace ruvia::detail {

[[nodiscard]] inline bool parseDecimalNumber(
    std::string_view text,
    double& output) noexcept {
    if (text == "inf" || text == "infinity") {
        output = std::numeric_limits<double>::infinity();
        return true;
    }
    if (text == "-inf" || text == "-infinity") {
        output = -std::numeric_limits<double>::infinity();
        return true;
    }
    if (text == "nan" || text == "-nan") {
        output = std::numeric_limits<double>::quiet_NaN();
        return true;
    }

    std::size_t index = 0;
    bool negative = false;
    if (index < text.size() && text[index] == '-') {
        negative = true;
        ++index;
    }

    std::uint64_t mantissa = 0;
    std::size_t keptDigits = 0;
    std::size_t droppedDigits = 0;
    std::size_t fractionalDigits = 0;
    int firstDroppedDigit = -1;
    bool sawDigit = false;
    bool sawNonZero = false;

    const auto consumeDigit = [&](char character, bool fractional) {
        const auto digit = static_cast<unsigned>(character - '0');
        sawDigit = true;
        if (fractional) {
            ++fractionalDigits;
        }
        if (!sawNonZero && digit == 0) {
            return;
        }
        sawNonZero = true;
        if (keptDigits < 19) {
            mantissa = mantissa * 10 + digit;
            ++keptDigits;
            return;
        }
        if (firstDroppedDigit < 0) {
            firstDroppedDigit = static_cast<int>(digit);
        }
        ++droppedDigits;
    };

    while (index < text.size() && text[index] >= '0' && text[index] <= '9') {
        consumeDigit(text[index], false);
        ++index;
    }
    if (index < text.size() && text[index] == '.') {
        ++index;
        const auto fractionBegin = index;
        while (index < text.size() && text[index] >= '0' && text[index] <= '9') {
            consumeDigit(text[index], true);
            ++index;
        }
        if (index == fractionBegin) {
            return false;
        }
    }
    if (!sawDigit) {
        return false;
    }

    int explicitExponent = 0;
    if (index < text.size() && (text[index] == 'e' || text[index] == 'E')) {
        ++index;
        bool exponentNegative = false;
        if (index < text.size() && (text[index] == '+' || text[index] == '-')) {
            exponentNegative = text[index] == '-';
            ++index;
        }
        const auto exponentBegin = index;
        while (index < text.size() && text[index] >= '0' && text[index] <= '9') {
            if (explicitExponent < 10000) {
                explicitExponent = explicitExponent * 10 + (text[index] - '0');
            }
            ++index;
        }
        if (index == exponentBegin) {
            return false;
        }
        if (exponentNegative) {
            explicitExponent = -explicitExponent;
        }
    }
    if (index != text.size()) {
        return false;
    }
    if (!sawNonZero) {
        output = negative ? -0.0 : 0.0;
        return true;
    }

    auto decimalExponent = static_cast<long long>(explicitExponent) -
        static_cast<long long>(fractionalDigits) +
        static_cast<long long>(droppedDigits);
    if (firstDroppedDigit >= 5) {
        ++mantissa;
        if (mantissa == 10000000000000000000ULL) {
            mantissa = 1000000000000000000ULL;
            ++decimalExponent;
        }
    }

    const auto magnitude = static_cast<long double>(mantissa) *
        std::pow(10.0L, static_cast<long double>(decimalExponent));
    const auto parsed = static_cast<double>(magnitude);
    if (!std::isfinite(parsed) || parsed == 0.0) {
        return false;
    }
    output = negative ? -parsed : parsed;
    return true;
}

}  // namespace ruvia::detail
