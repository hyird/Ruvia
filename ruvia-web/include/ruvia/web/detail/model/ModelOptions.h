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
    static constexpr bool nullable = (std::is_same_v<OptionTs, Nullable> || ... || false);

    constexpr ModelOptions() noexcept
        : options_(OptionTs{}...) {
        static_assert((isModelOption<OptionTs>() && ... && true),
            "model field options must be RUVIA_DEFAULT, RUVIA_NULLABLE, RUVIA_OMIT_EMPTY, or RUVIA_EMIT_NULL");
    }

    [[nodiscard]] constexpr bool emitNull() const noexcept {
        return containsOption<EmitNull>();
    }

    [[nodiscard]] constexpr bool omitEmpty() const noexcept {
        return containsOption<OmitEmpty>();
    }

    template <typename OptionalT>
    void applyDefault(OptionalT& value, std::pmr::memory_resource* resource) const {
        if (value) {
            return;
        }
        std::apply(
            [&value, resource](
                const auto&... options) { (applyDefaultOption(value, resource, options), ...); },
            options_);
    }

private:
    template <typename OptionT>
    [[nodiscard]] static constexpr bool containsOption() noexcept {
        return (std::is_same_v<std::remove_cvref_t<OptionTs>, OptionT> || ... || false);
    }

    template <typename OptionalT, typename OptionT>
    static void applyDefaultOption(
        OptionalT& value, std::pmr::memory_resource* resource, const OptionT& option) {
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
        std::optional<FieldT>& target, const ValueT& value, std::pmr::memory_resource* resource) {
        if constexpr (detail::isRuviaString<FieldT> &&
                      std::is_convertible_v<const ValueT&, std::string_view>) {
            target.emplace(std::string_view(value), ::ruvia::ModelOptions{.resource = resource});
        } else {
            target.emplace(value);
        }
    }

    std::tuple<OptionTs...> options_;
};

}  // namespace ruvia::detail::model
