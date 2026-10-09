#include "ruvia/http/HttpDate.h"

#include <variant>

#include "field/HttpImfFixdate.h"

namespace ruvia {

std::optional<std::array<char, 29>> formatHttpDate(std::time_t value) noexcept {
    const auto date = detail::httpFormatDate(value);
    if ((date.index() != 0)) {
        return std::nullopt;
    }
    return std::get<0>(date);
}

}  // namespace ruvia
