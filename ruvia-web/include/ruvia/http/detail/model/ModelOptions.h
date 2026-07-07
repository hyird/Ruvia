#pragma once

#include <memory_resource>
#include <optional>
#include <string_view>
#include <tuple>
#include <type_traits>

#include "ruvia/http/detail/model/RuleSupport.h"

namespace ruvia::detail::model {

template <typename... OptionTs>
class ModelOptions final {
public:
    constexpr explicit ModelOptions(OptionTs... options) noexcept : options_(options...) {
        static_assert((isModelOption<OptionTs>() && ...),
            "RUVIA_FIELD accepts only model options: RUVIA_DEFAULT, RUVIA_OMIT_EMPTY, RUVIA_EMIT_NULL. "
            "Move validation rules to RUVIA_VALIDATE_JSON or RUVIA_VALIDATE_FORM with RUVIA_RULE.");
    }

    [[nodiscard]] constexpr bool emitNull() const noexcept {
        return containsOption<EmitNull>(std::index_sequence_for<OptionTs...>{});
    }

    [[nodiscard]] constexpr bool omitEmpty() const noexcept {
        return containsOption<OmitEmpty>(std::index_sequence_for<OptionTs...>{});
    }

    template <typename OptionalT>
    void applyDefault(OptionalT& value, std::pmr::memory_resource* resource) const {
        if (value) {
            return;
        }
        std::apply(
            [&value, resource](const auto&... options) {
                (applyDefaultOption(value, resource, options), ...);
            },
            options_);
    }

private:
    template <typename OptionT, std::size_t... Indexes>
    [[nodiscard]] constexpr bool containsOption(std::index_sequence<Indexes...>) const noexcept {
        return ((std::is_same_v<std::remove_cvref_t<decltype(std::get<Indexes>(options_))>, OptionT>) || ... || false);
    }

    template <typename OptionalT, typename OptionT>
    static void applyDefaultOption(OptionalT& value, std::pmr::memory_resource* resource, const OptionT& option) {
        if constexpr (isDefaultRule<OptionT>()) {
            using FieldT = typename OptionalT::value_type;
            assignDefaultValue<FieldT>(value, option.value, resource);
        } else {
            (void)value;
            (void)resource;
            (void)option;
        }
    }

    template <typename FieldT, typename ValueT>
    static void assignDefaultValue(
        std::optional<FieldT>& target,
        const ValueT& value,
        std::pmr::memory_resource* resource) {
        if constexpr (detail::isRuviaString<FieldT> && std::is_convertible_v<const ValueT&, std::string_view>) {
            target.emplace(std::string_view(value), resource);
        } else {
            target.emplace(value);
        }
    }

    std::tuple<OptionTs...> options_;
};

template <typename... OptionTs>
ModelOptions(OptionTs...) -> ModelOptions<OptionTs...>;

}  // namespace ruvia::detail::model
