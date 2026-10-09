#pragma once

#include <charconv>
#include <concepts>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <variant>

namespace ruvia {

enum class integer_parse_error {
    invalid_format,
    out_of_range,
};

// Strict decimal input: no whitespace, leading '+', or trailing characters.
// A leading '-' is accepted only for signed integers. Format errors take
// precedence over overflow when the input also contains trailing characters.
template <std::integral t_type>
    requires(!std::same_as<std::remove_cv_t<t_type>, bool> && requires(const char* first, const char* last, t_type& value) {
        std::from_chars(first, last, value, 10);
    })
[[nodiscard]] constexpr std::variant<t_type, integer_parse_error> parse_integer(std::string_view text) noexcept {
    if (text.empty()) {
        return integer_parse_error::invalid_format;
    }
    t_type value{};
    const auto* end = text.data() + text.size();
    const auto [ptr, error] = std::from_chars(text.data(), end, value, 10);
    if (error == std::errc::invalid_argument || ptr != end) {
        return integer_parse_error::invalid_format;
    }
    if (error == std::errc::result_out_of_range) {
        return integer_parse_error::out_of_range;
    }
    return value;
}

}  // namespace ruvia
