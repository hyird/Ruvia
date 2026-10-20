#include "ruvia/http/http_date.h"

#include <variant>

#include "ruvia/http/http_field_whitespace.h"

#include "field/http_date.h"
#include "field/http_imf_fixdate.h"

namespace ruvia {

std::optional<std::array<char, 29>> format_http_date(std::time_t value) noexcept {
    const auto date = detail::http_format_date(value);
    if ((date.index() != 0)) {
        return std::nullopt;
    }
    return std::get<0>(date);
}

std::optional<std::time_t> parse_http_date(std::string_view value) noexcept {
    return detail::http_parse_http_date(http_trim_ows(value));
}

}  // namespace ruvia
