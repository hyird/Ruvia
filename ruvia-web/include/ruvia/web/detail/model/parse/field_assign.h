#pragma once

#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "ruvia/web/detail/model/rule/rule_support.h"

namespace ruvia::detail::model {

template <typename field_t_type, typename value_t_type>
void assign_field_value(
    std::optional<field_t_type>& target, value_t_type&& value, std::pmr::memory_resource* resource) {
    if constexpr (detail::is_ruvia_scalar<field_t_type>) {
        // Validate narrowing before replacing an existing field value.
        const auto field = [&] {
            if constexpr (detail::is_ruvia_scalar<value_t_type>) {
                return field_t_type(value.value_);
            } else {
                return field_t_type(std::forward<value_t_type>(value));
            }
        }();
        target.emplace(field);
    } else if constexpr (detail::is_ruvia_string<field_t_type> &&
                         std::is_same_v<std::remove_cvref_t<value_t_type>, std::pmr::string> &&
                         std::is_rvalue_reference_v<value_t_type&&>) {
        field_t_type field(::ruvia::model_options{.resource_ = resource});
        field.assign_owned(std::forward<value_t_type>(value));
        target.emplace(std::move(field));
    } else if constexpr (detail::is_ruvia_string<field_t_type> &&
                         std::is_convertible_v<value_t_type&&, std::string_view> &&
                         !std::is_same_v<std::remove_cvref_t<value_t_type>, field_t_type>) {
        field_t_type field(::ruvia::model_options{.resource_ = resource});
        field.assign_owned(std::string_view(std::forward<value_t_type>(value)));
        target.emplace(std::move(field));
    } else if constexpr (detail::is_ruvia_bytes<field_t_type> &&
                         !std::is_same_v<std::remove_cvref_t<value_t_type>, field_t_type>) {
        field_t_type field(::ruvia::model_options{.resource_ = resource});
        if constexpr (std::is_same_v<std::remove_cvref_t<value_t_type>, std::pmr::vector<std::uint8_t>> &&
                      std::is_rvalue_reference_v<value_t_type&&>) {
            field.assign_owned(std::forward<value_t_type>(value));
        } else {
            field.assign_owned(std::span<const std::uint8_t>(std::forward<value_t_type>(value)));
        }
        target.emplace(std::move(field));
    } else {
        target.emplace(detail::rebind_model_value(std::forward<value_t_type>(value), resource));
    }
}

}  // namespace ruvia::detail::model
