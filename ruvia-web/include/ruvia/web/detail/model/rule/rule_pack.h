#pragma once

#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/web/detail/model/rule/rule_validation.h"
#include "ruvia/web/model.h"

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
        [[maybe_unused]] const model_path& path, [[maybe_unused]] validator_type& validator_value) const {
        if (state_value != model_field_state::parsed || !value) {
            return;
        }
        (validate_rule(*value, path, validator_value, rule_types{}), ...);
    }
};

}  // namespace ruvia::detail::model
