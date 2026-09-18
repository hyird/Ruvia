#pragma once

#include <charconv>
#include <concepts>
#include <expected>
#include <string_view>
#include <system_error>
#include <type_traits>

namespace ruvia {

enum class IntegerParseError {
    kInvalidFormat,
    kOutOfRange,
};

// Strict decimal input: no whitespace, leading '+', or trailing characters.
// A leading '-' is accepted only for signed integers. Format errors take
// precedence over overflow when the input also contains trailing characters.
template <std::integral T>
    requires(!std::same_as<std::remove_cv_t<T>, bool> && requires(const char* first, const char* last, T& value) {
        std::from_chars(first, last, value, 10);
    })
[[nodiscard]] constexpr std::expected<T, IntegerParseError> parseInteger(std::string_view text) noexcept {
    if (text.empty()) {
        return std::unexpected(IntegerParseError::kInvalidFormat);
    }
    T value{};
    const auto* end = text.data() + text.size();
    const auto [ptr, error] = std::from_chars(text.data(), end, value, 10);
    if (error == std::errc::invalid_argument || ptr != end) {
        return std::unexpected(IntegerParseError::kInvalidFormat);
    }
    if (error == std::errc::result_out_of_range) {
        return std::unexpected(IntegerParseError::kOutOfRange);
    }
    return value;
}

}  // namespace ruvia
