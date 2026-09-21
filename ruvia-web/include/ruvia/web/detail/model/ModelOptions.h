#pragma once

#include <type_traits>

#include "ruvia/web/detail/model/rule/RuleSupport.h"

namespace ruvia::detail::model {

// Field options are type-level metadata, not per-model runtime objects.
template <typename... OptionTs>
class ModelOptions final {
    static_assert((isModelOption<OptionTs>() && ... && true),
        "model field options must be RUVIA_DEFAULT, RUVIA_INITIAL, RUVIA_NULLABLE, RUVIA_OMIT_EMPTY, or RUVIA_EMIT_NULL");
    static_assert(((isDefaultRule<OptionTs>() ? 1 : 0) + ... + 0) <= 1,
        "a model field may have at most one RUVIA_DEFAULT");
    static_assert(((isInitialRule<OptionTs>() ? 1 : 0) + ... + 0) <= 1,
        "a model field may have at most one RUVIA_INITIAL");

public:
    static constexpr bool nullable = (std::is_same_v<OptionTs, Nullable> || ... || false);

    [[nodiscard]] static constexpr bool emitNull() noexcept {
        return containsOption<EmitNull>();
    }

    [[nodiscard]] static constexpr bool omitEmpty() noexcept {
        return containsOption<OmitEmpty>();
    }

    static constexpr bool hasInitial = (isInitialRule<OptionTs>() || ... || false);

    // The field owns assignment, nullability and allocator normalization.
    template <typename ConsumerT>
    static void applyDefault(ConsumerT&& consume) {
        if constexpr (sizeof...(OptionTs) != 0) {
            (applyDefaultOption<OptionTs>(consume), ...);
        } else {
            (void)consume;
        }
    }

    template <typename ConsumerT>
    static void applyInitial(ConsumerT&& consume) {
        if constexpr (sizeof...(OptionTs) != 0) {
            (applyInitialOption<OptionTs>(consume), ...);
        } else {
            (void)consume;
        }
    }

private:
    template <typename OptionT>
    [[nodiscard]] static constexpr bool containsOption() noexcept {
        return (std::is_same_v<std::remove_cvref_t<OptionTs>, OptionT> || ... || false);
    }

    template <typename OptionT, typename ConsumerT>
    static void applyInitialOption(ConsumerT& consume) {
        if constexpr (isInitialRule<OptionT>()) {
            consume(OptionT::value());
        } else {
            (void)consume;
        }
    }

    template <typename OptionT, typename ConsumerT>
    static void applyDefaultOption(ConsumerT& consume) {
        if constexpr (isDefaultRule<OptionT>()) {
            consume(OptionT::value());
        } else {
            (void)consume;
        }
    }
};

}  // namespace ruvia::detail::model
