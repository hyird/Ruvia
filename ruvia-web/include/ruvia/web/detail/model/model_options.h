#pragma once

#include <type_traits>

#include "ruvia/web/detail/model/rule/rule_support.h"

namespace ruvia::detail::model {

// Field options are type-level metadata, not per-model runtime objects.
template <typename... option_ts_type>
class model_options final {
    static_assert((is_model_option<option_ts_type>() && ... && true),
        "model field options must be RUVIA_DEFAULT, RUVIA_INITIAL, RUVIA_NULLABLE, RUVIA_OMIT_EMPTY, or RUVIA_EMIT_NULL");
    static_assert(((is_default_rule<option_ts_type>() ? 1 : 0) + ... + 0) <= 1,
        "a model field may have at most one RUVIA_DEFAULT");
    static_assert(((is_initial_rule<option_ts_type>() ? 1 : 0) + ... + 0) <= 1,
        "a model field may have at most one RUVIA_INITIAL");

public:
    static constexpr bool nullable = (std::is_same_v<option_ts_type, ::ruvia::detail::model::nullable> || ... || false);

    [[nodiscard]] static constexpr bool emit_null() noexcept {
        return contains_option<::ruvia::detail::model::emit_null>();
    }

    [[nodiscard]] static constexpr bool omit_empty() noexcept {
        return contains_option<::ruvia::detail::model::omit_empty>();
    }

    static constexpr bool has_initial = (is_initial_rule<option_ts_type>() || ... || false);

    // The field owns assignment, nullability and allocator normalization.
    template <typename consumer_t_type>
    static void apply_default(consumer_t_type&& consume) {
        if constexpr (sizeof...(option_ts_type) != 0) {
            (apply_default_option<option_ts_type>(consume), ...);
        } else {
            (void)consume;
        }
    }

    template <typename consumer_t_type>
    static void apply_initial(consumer_t_type&& consume) {
        if constexpr (sizeof...(option_ts_type) != 0) {
            (apply_initial_option<option_ts_type>(consume), ...);
        } else {
            (void)consume;
        }
    }

private:
    template <typename option_t_type>
    [[nodiscard]] static constexpr bool contains_option() noexcept {
        return (std::is_same_v<std::remove_cvref_t<option_ts_type>, option_t_type> || ... || false);
    }

    template <typename option_t_type, typename consumer_t_type>
    static void apply_initial_option(consumer_t_type& consume) {
        if constexpr (is_initial_rule<option_t_type>()) {
            consume(option_t_type::value());
        } else {
            (void)consume;
        }
    }

    template <typename option_t_type, typename consumer_t_type>
    static void apply_default_option(consumer_t_type& consume) {
        if constexpr (is_default_rule<option_t_type>()) {
            consume(option_t_type::value());
        } else {
            (void)consume;
        }
    }
};

}  // namespace ruvia::detail::model
