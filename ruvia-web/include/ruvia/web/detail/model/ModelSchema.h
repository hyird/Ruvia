#pragma once

#include <array>
#include <cstdint>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#include "ruvia/web/detail/model/ModelField.h"
#include "ruvia/web/detail/model/Traits.h"

namespace ruvia::detail::model {

template <FixedString SourceName, FixedString WireName, typename ValueT, bool Required, typename... OptionTs>
struct ModelFieldDescriptor final {
    using value_type = ValueT;
    using field_type = ModelField<ValueT, Required, ModelOptions<OptionTs...>, WireName>;

    static constexpr auto sourceName = SourceName;
    static constexpr auto wireName = WireName;
    static constexpr auto wireHash = modelFieldNameHash(WireName.view());
    static constexpr bool required = Required;
};

template <typename... DescriptorTs>
struct ModelSchema final {};

template <FixedString Field, typename... DescriptorTs>
[[nodiscard]] consteval std::size_t modelFieldIndex() {
    constexpr std::size_t matches = (std::size_t{Field == DescriptorTs::sourceName} + ... + 0);
    static_assert(matches == 1, "unknown or duplicate Ruvia model field");

    constexpr std::array<bool, sizeof...(DescriptorTs)> fields{Field == DescriptorTs::sourceName...};
    for (std::size_t index = 0; index < fields.size(); ++index) {
        if (fields[index]) {
            return index;
        }
    }
    return 0;
}

template <typename... DescriptorTs>
[[nodiscard]] consteval bool uniqueModelFieldNames() {
    constexpr std::array<std::string_view, sizeof...(DescriptorTs)> names{DescriptorTs::sourceName.view()...};
    for (std::size_t left = 0; left < names.size(); ++left) {
        for (std::size_t right = left + 1; right < names.size(); ++right) {
            if (names[left] == names[right]) {
                return false;
            }
        }
    }
    return true;
}

template <typename... DescriptorTs>
[[nodiscard]] consteval bool uniqueModelWireNames() {
    constexpr std::array<std::string_view, sizeof...(DescriptorTs)> names{DescriptorTs::wireName.view()...};
    for (std::size_t left = 0; left < names.size(); ++left) {
        for (std::size_t right = left + 1; right < names.size(); ++right) {
            if (names[left] == names[right]) {
                return false;
            }
        }
    }
    return true;
}

template <typename ModelT, typename... DescriptorTs, typename VisitorT, std::size_t... Indices>
constexpr void visitModelFieldsImpl(
    ModelT& model,
    ModelSchema<DescriptorTs...>,
    VisitorT&& visitor,
    std::index_sequence<Indices...>) {
    auto& visitorRef = visitor;
    (visitorRef(DescriptorTs{}, model.template ruviaSlot<Indices>()), ...);
}

template <typename ModelT, typename... DescriptorTs, typename VisitorT>
constexpr void visitModelFields(
    ModelT& model,
    ModelSchema<DescriptorTs...> schema,
    VisitorT&& visitor) {
    visitModelFieldsImpl(
        model,
        schema,
        std::forward<VisitorT>(visitor),
        std::index_sequence_for<DescriptorTs...>{});
}

template <std::size_t Index, typename DescriptorT, typename... RemainingTs, typename ModelT, typename VisitorT>
[[nodiscard]] bool visitModelFieldByWireNameImpl(
    ModelT& model,
    std::uint64_t wireHash,
    std::string_view wireName,
    bool& visitResult,
    VisitorT& visitor) {
    if (wireHash == DescriptorT::wireHash && wireName == DescriptorT::wireName.view()) {
        visitResult = visitor(model.template ruviaSlot<Index>());
        return true;
    }
    if constexpr (sizeof...(RemainingTs) > 0) {
        return visitModelFieldByWireNameImpl<Index + 1, RemainingTs...>(
            model,
            wireHash,
            wireName,
            visitResult,
            visitor);
    } else {
        return false;
    }
}

template <typename ModelT, typename... DescriptorTs, typename VisitorT>
[[nodiscard]] bool visitModelFieldByWireName(
    ModelT& model,
    ModelSchema<DescriptorTs...>,
    std::uint64_t wireHash,
    std::string_view wireName,
    bool& visitResult,
    VisitorT&& visitor) {
    if constexpr (sizeof...(DescriptorTs) == 0) {
        return false;
    } else {
        auto& visitorRef = visitor;
        return visitModelFieldByWireNameImpl<0, DescriptorTs...>(
            model,
            wireHash,
            wireName,
            visitResult,
            visitorRef);
    }
}

template <FixedString Field, typename ModelT, typename... DescriptorTs>
[[nodiscard]] ModelFieldState modelFieldState(
    const ModelT& model,
    ModelSchema<DescriptorTs...>) {
    constexpr auto index = modelFieldIndex<Field, DescriptorTs...>();
    return model.template ruviaSlot<index>().state();
}

}  // namespace ruvia::detail::model
