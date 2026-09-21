#pragma once

#include <optional>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#include "ruvia/web/detail/model/Traits.h"
#include "ruvia/web/detail/model/rule/RuleValidation.h"

namespace ruvia::detail::model {

template <typename... RuleTs>
class Rules final {
public:
    constexpr Rules() noexcept {
        static_assert((isValidationRule<RuleTs>() && ... && true),
            "field rules must be RUVIA_MIN, RUVIA_MAX, RUVIA_ONE_OF, RUVIA_EMAIL, RUVIA_PATTERN, "
            "RUVIA_REGEX, or RUVIA_CUSTOM");
    }

    template <typename ValueT, typename ValidatorT>
    void validate(ModelFieldState state, const std::optional<ValueT>& value,
        std::string_view path, ValidatorT& validator) const {
        if (state != ModelFieldState::kParsed || !value) {
            return;
        }
        validatePresent(*value, path, validator);
    }

private:
    template <typename ValueT, typename ValidatorT>
    void validatePresent(const ValueT& value, std::string_view path, ValidatorT& validator) const {
        std::apply(
            [&value, path, &validator](
                const auto&... rules) { (validateRule(value, path, validator, rules), ...); },
            rules_);
    }

    std::tuple<RuleTs...> rules_{};
};

}  // namespace ruvia::detail::model
