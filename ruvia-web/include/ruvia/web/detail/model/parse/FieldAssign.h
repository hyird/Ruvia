#pragma once

#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/web/detail/model/rule/RuleSupport.h"

namespace ruvia::detail::model {

template <typename FieldT, typename ValueT>
void assignFieldValue(
    std::optional<FieldT>& target, ValueT&& value, std::pmr::memory_resource* resource) {
    if constexpr (detail::isRuviaString<FieldT> &&
                  std::is_same_v<std::remove_cvref_t<ValueT>, std::pmr::string> &&
                  std::is_rvalue_reference_v<ValueT&&>) {
        FieldT field(::ruvia::ModelOptions{.resource = resource});
        field.assignOwned(std::forward<ValueT>(value));
        target.emplace(std::move(field));
    } else if constexpr (detail::isRuviaString<FieldT> &&
                         std::is_convertible_v<ValueT&&, std::string_view> &&
                         !std::is_same_v<std::remove_cvref_t<ValueT>, FieldT>) {
        FieldT field(::ruvia::ModelOptions{.resource = resource});
        field.assignOwned(std::string_view(std::forward<ValueT>(value)));
        target.emplace(std::move(field));
    } else {
        target.emplace(std::forward<ValueT>(value));
    }
}

}  // namespace ruvia::detail::model
