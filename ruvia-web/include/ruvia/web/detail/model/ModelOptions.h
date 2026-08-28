#pragma once

#include <memory_resource>
#include <optional>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#include "ruvia/web/detail/model/rule/RuleSupport.h"

namespace ruvia::detail::model {

template <typename... OptionTs>
class ModelOptions final {
public:
    constexpr ModelOptions() noexcept
        : options_(OptionTs{}...) {
        static_assert((isModelOption<OptionTs>() && ...),
            "RUVIA_REQUIRED_FIELD/RUVIA_OPTIONAL_FIELD accept only model options: RUVIA_DEFAULT, "
            "RUVIA_OMIT_EMPTY, "
            "RUVIA_EMIT_NULL. "
            "Move validation rules to RUVIA_VALIDATE_* with RUVIA_RULE.");
    }

    [[nodiscard]] constexpr bool emitNull() const noexcept {
        return containsOption<EmitNull>();
    }

    [[nodiscard]] constexpr bool omitEmpty() const noexcept {
        return containsOption<OmitEmpty>();
    }

    [[nodiscard]] constexpr bool hasDefault() const noexcept {
        return containsDefault();
    }

    template <typename OptionalT>
    void applyDefault(OptionalT& value, std::pmr::memory_resource* resource) const {
        if (value) {
            return;
        }
        std::apply([&value, resource](const auto&... options) { (applyDefaultOption(value, resource, options), ...); }, options_);
    }

private:
    template <typename OptionT>
    [[nodiscard]] static constexpr bool containsOption() noexcept {
        return (std::is_same_v<std::remove_cvref_t<OptionTs>, OptionT> || ... || false);
    }

    [[nodiscard]] static constexpr bool containsDefault() noexcept {
        return (isDefaultRule<std::remove_cvref_t<OptionTs>>() || ... || false);
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
    static void assignDefaultValue(std::optional<FieldT>& target, const ValueT& value, std::pmr::memory_resource* resource) {
        if constexpr (detail::isRuviaString<FieldT> && std::is_convertible_v<const ValueT&, std::string_view>) {
            target.emplace(std::string_view(value), ::ruvia::ModelOptions{.resource = resource});
        } else {
            target.emplace(value);
        }
    }

    std::tuple<OptionTs...> options_;
};

}  // namespace ruvia::detail::model
