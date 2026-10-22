#pragma once

#include <cstdint>
#include <memory>
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

template <typename field_type, typename value_type>
void assign_field_value(
    std::optional<field_type>& target, value_type&& value, std::pmr::memory_resource* resource) {
    if constexpr (detail::is_ruvia_scalar<field_type>) {
        // Validate narrowing before replacing an existing field value.
        const auto field = [&] {
            if constexpr (detail::is_ruvia_scalar<value_type>) {
                return field_type(value.value_);
            } else {
                return field_type(std::forward<value_type>(value));
            }
        }();
        target.emplace(field);
    } else if constexpr (detail::is_ruvia_string<field_type> &&
                         std::is_same_v<std::remove_cvref_t<value_type>, std::pmr::string> &&
                         std::is_rvalue_reference_v<value_type&&>) {
        field_type field(::ruvia::model_options{.resource_ = resource});
        field.assign_owned(std::forward<value_type>(value));
        target.emplace(std::move(field));
    } else if constexpr (detail::is_ruvia_string<field_type> &&
                         std::is_convertible_v<value_type&&, std::string_view> &&
                         !std::is_same_v<std::remove_cvref_t<value_type>, field_type>) {
        field_type field(::ruvia::model_options{.resource_ = resource});
        field.assign_owned(std::string_view(std::forward<value_type>(value)));
        target.emplace(std::move(field));
    } else if constexpr (detail::is_ruvia_bytes<field_type> &&
                         !std::is_same_v<std::remove_cvref_t<value_type>, field_type>) {
        field_type field(::ruvia::model_options{.resource_ = resource});
        if constexpr (std::is_same_v<std::remove_cvref_t<value_type>, std::pmr::vector<std::uint8_t>> &&
                      std::is_rvalue_reference_v<value_type&&>) {
            field.assign_owned(std::forward<value_type>(value));
        } else {
            field.assign_owned(std::span<const std::uint8_t>(std::forward<value_type>(value)));
        }
        target.emplace(std::move(field));
    } else {
        if constexpr (!std::is_lvalue_reference_v<value_type&&> &&
                      !std::is_const_v<std::remove_reference_t<value_type>>) {
            // Consuming a source that owns this field would destroy the destination.
            if (model_value_rebind_access::owns(value, std::addressof(target))) {
                throw_model_move_source_owns_destination();
            }
        }
        target.emplace(detail::rebind_model_value(std::forward<value_type>(value), resource));
    }
}

}  // namespace ruvia::detail::model
