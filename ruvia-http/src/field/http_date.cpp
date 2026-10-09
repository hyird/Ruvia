#include "ruvia/http/http_date.h"

#include <variant>

#include "field/http_imf_fixdate.h"

namespace ruvia {

std::optional<std::array<char, 29>> format_http_date(std::time_t value) noexcept {
    const auto date = detail::http_format_date(value);
    if ((date.index() != 0)) {
        return std::nullopt;
    }
    return std::get<0>(date);
}

}  // namespace ruvia
