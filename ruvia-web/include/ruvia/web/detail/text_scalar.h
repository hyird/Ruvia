#pragma once

#include <charconv>
#include <cmath>
#include <memory_resource>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>

#include "ruvia/web/integer.h"
#include "ruvia/web/model.h"

namespace ruvia::detail {

enum class text_scalar_parse_error {
    invalid_format,
    out_of_range,
};

template <typename t_type>
concept text_scalar = std::is_same_v<t_type, std::remove_cv_t<t_type>> &&
                      (std::is_same_v<t_type, bool> || is_model_integer<t_type> ||
                          std::is_floating_point_v<t_type> || std::is_same_v<t_type, std::pmr::string> ||
                          std::is_same_v<t_type, string> || std::is_same_v<t_type, std::string_view>);

// The one text-to-scalar syntax shared by database results and Redis replies:
// boolean tokens 1/t/true/TRUE and 0/f/false/FALSE, strict decimal integers
// (parse_integer), and finite floating-point values. Owning strings allocate
// from `resource`; std::string_view borrows `text`. A float that is
// syntactically complete but not representable is out_of_range; any other
// rejection, including a non-finite value, is invalid_format.
template <text_scalar t_type>
[[nodiscard]] std::variant<t_type, text_scalar_parse_error> parse_text_scalar(
    std::string_view text, std::pmr::memory_resource* resource) {
    using result_type = std::variant<t_type, text_scalar_parse_error>;
    if constexpr (std::is_same_v<t_type, std::string_view>) {
        return result_type(std::in_place_index<0>, text);
    } else if constexpr (std::is_same_v<t_type, std::pmr::string>) {
        return result_type(std::in_place_index<0>, text, pmr_resource_or_default(resource));
    } else if constexpr (std::is_same_v<t_type, string>) {
        return result_type(std::in_place_index<0>, text, model_options{.resource_ = resource});
    } else if constexpr (std::is_same_v<t_type, bool>) {
        if (text == "1" || text == "t" || text == "true" || text == "TRUE") {
            return result_type(std::in_place_index<0>, true);
        }
        if (text == "0" || text == "f" || text == "false" || text == "FALSE") {
            return result_type(std::in_place_index<0>, false);
        }
        return text_scalar_parse_error::invalid_format;
    } else if constexpr (std::is_floating_point_v<t_type>) {
        t_type converted{};
        const auto* const last = text.data() + text.size();
        const auto [end, error] = std::from_chars(text.data(), last, converted);
        if (error == std::errc::result_out_of_range && end == last) {
            return text_scalar_parse_error::out_of_range;
        }
        if (error != std::errc{} || end != last || !std::isfinite(converted)) {
            return text_scalar_parse_error::invalid_format;
        }
        return result_type(std::in_place_index<0>, converted);
    } else {
        const auto parsed = parse_integer<t_type>(text);
        if (const auto* error = std::get_if<integer_parse_error>(&parsed)) {
            return *error == integer_parse_error::out_of_range ? text_scalar_parse_error::out_of_range
                                                               : text_scalar_parse_error::invalid_format;
        }
        return result_type(std::in_place_index<0>, std::get<t_type>(parsed));
    }
}

}  // namespace ruvia::detail
