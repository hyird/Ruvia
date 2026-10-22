#pragma once

#include <optional>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#include "ruvia/web/detail/model/rule/rule_validation.h"
#include "ruvia/web/detail/model/traits.h"

namespace ruvia::detail::model {

template <typename... rule_types>
class rules final {
public:
    constexpr rules() noexcept {
        static_assert((is_validation_rule<rule_types>() && ... && true),
            "field rules must be RUVIA_MIN, RUVIA_MAX, RUVIA_ONE_OF, RUVIA_EMAIL, RUVIA_PATTERN, "
            "or RUVIA_CUSTOM");
    }

    template <typename value_type, typename validator_type>
    void validate(model_field_state state_value, const std::optional<value_type>& value,
        const model_path& path, validator_type& validator_value) const {
        if (state_value != model_field_state::parsed || !value) {
            return;
        }
        validate_present(*value, path, validator_value);
    }

private:
    template <typename value_type, typename validator_type>
    void validate_present(const value_type& value, const model_path& path, validator_type& validator_value) const {
        std::apply(
            [&value, &path, &validator_value](
                const auto&... rules) { ((validator_value.full() ? void() : validate_rule(value, path, validator_value, rules)), ...); },
            rules_);
    }

    std::tuple<rule_types...> rules_{};
};

}  // namespace ruvia::detail::model
