#include "ruvia/http/HttpDate.h"

#include "ruvia/http/detail/field/HttpImfFixdate.h"

namespace ruvia {

std::optional<std::array<char, 29>> formatHttpDate(std::time_t value) noexcept {
    const auto date = detail::httpFormatDate(value);
    if (!date) {
        return std::nullopt;
    }
    return *date;
}

}  // namespace ruvia
