#pragma once

#include <optional>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#include "ruvia/web/detail/model/rule/rule_validation.h"
#include "ruvia/web/detail/model/traits.h"

namespace ruvia::detail::model {

template <typename... rule_ts_type>
class rules final {
public:
    constexpr rules() noexcept {
        static_assert((is_validation_rule<rule_ts_type>() && ... && true),
            "field rules must be RUVIA_MIN, RUVIA_MAX, RUVIA_ONE_OF, RUVIA_EMAIL, RUVIA_PATTERN, "
            "RUVIA_REGEX, or RUVIA_CUSTOM");
    }

    template <typename value_t_type, typename validator_t_type>
    void validate(model_field_state state_value, const std::optional<value_t_type>& value,
        std::string_view path, validator_t_type& validator_value) const {
        if (state_value != model_field_state::parsed || !value) {
            return;
        }
        validate_present(*value, path, validator_value);
    }

private:
    template <typename value_t_type, typename validator_t_type>
    void validate_present(const value_t_type& value, std::string_view path, validator_t_type& validator_value) const {
        std::apply(
            [&value, path, &validator_value](
                const auto&... rules) { ((validator_value.full() ? void() : validate_rule(value, path, validator_value, rules)), ...); },
            rules_);
    }

    std::tuple<rule_ts_type...> rules_{};
};

}  // namespace ruvia::detail::model
