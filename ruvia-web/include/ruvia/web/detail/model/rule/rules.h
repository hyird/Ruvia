#pragma once

#include <optional>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#include "ruvia/web/detail/model/model_schema.h"
#include "ruvia/web/detail/model/rule/rule_pack.h"

namespace ruvia::detail {

struct model_validation_access final {
    template <fixed_string field, typename model_t_type>
    [[nodiscard]] static model_field_state field_state(const model_t_type& model) {
        return model::model_field_state<field>(model, model::model_access::schema<model_t_type>());
    }

    template <fixed_string field, typename model_t_type>
    [[nodiscard]] static const auto& field_value(const model_t_type& model) {
        const auto& slot = [&]<typename... descriptor_types>(model::model_schema<descriptor_types...>) -> const auto& {
            constexpr auto index = model::model_field_index<field, descriptor_types...>();
            return model::model_access::slot<index>(model);
        }(model::model_access::schema<model_t_type>());
        return slot.value();
    }

    template <typename model_t_type>
    [[nodiscard]] static bool structure_valid(const model_t_type& model) {
        bool valid = true;
        model::visit_model_fields(model, model::model_access::schema<model_t_type>(), [&](const auto&, const auto& slot) {
            using slot_t_type = std::remove_cvref_t<decltype(slot)>;
            const auto state_value = slot.state();
            if (state_value == model_field_state::duplicate || state_value == model_field_state::invalid_type ||
                (slot_t_type::required && state_value == model_field_state::missing)) {
                valid = false;
                return;
            }
            if (const auto& value = slot.value(); value && !value_structure_valid(*value)) {
                valid = false;
            }
        });
        return valid;
    }

    template <typename model_t_type, typename validator_t_type>
    static void validate_structure(
        const model_t_type& model_value, std::string_view prefix, validator_t_type& validator_value) {
        model::visit_model_fields(
            model_value, model::model_access::schema<model_t_type>(), [&](const auto&, const auto& slot) {
                if (validator_value.full()) {
                    return;
                }
                using slot_t_type = std::remove_cvref_t<decltype(slot)>;
                std::pmr::string path(validator_value.resource());
                model::append_path(path, prefix, slot.wire_name());

                switch (slot.state()) {
                    case model_field_state::duplicate:
                        validator_value.add(path, "duplicate", "is duplicated");
                        return;
                    case model_field_state::invalid_type:
                        validator_value.add(path, "invalid_type",
                            model::expected_type_name<typename slot_t_type::value_type>());
                        return;
                    case model_field_state::missing:
                        if constexpr (slot_t_type::required) {
                            validator_value.add(path, "required", "is required");
                        }
                        return;
                    case model_field_state::null:
                        return;
                    case model_field_state::parsed:
                        break;
                }

                validate_value_structure(*slot.value(), path, validator_value);
            });
    }

    template <typename model_t_type, typename validator_t_type>
    static void validate_model(const model_t_type& model_value, validator_t_type& validator_value) {
        validate_structure(model_value, {}, validator_value);
        validate_field_rules(model_value, {}, validator_value);
    }

    template <typename model_t_type, typename validator_t_type>
    static void validate_field_rules(
        const model_t_type& model_value, std::string_view prefix, validator_t_type& validator_value) {
        model::visit_model_fields(
            model_value, model::model_access::schema<model_t_type>(), [&](const auto& descriptor, const auto& slot) {
                if (validator_value.full()) {
                    return;
                }
                using descriptor_t_type = std::remove_cvref_t<decltype(descriptor)>;
                std::pmr::string path(validator_value.resource());
                model::append_path(path, prefix, slot.wire_name());
                typename descriptor_t_type::rules_type{}.validate(
                    slot.state(), slot.value(), path, validator_value);

                if (slot.state() != model_field_state::parsed || !slot.value()) {
                    return;
                }
                validate_nested_field_rules(*slot.value(), path, validator_value);
            });
    }

    template <typename value_t_type>
    [[nodiscard]] static bool value_structure_valid(const value_t_type& value) {
        using t_type = std::remove_cvref_t<value_t_type>;
        if constexpr (is_model<t_type>) {
            return structure_valid(value);
        } else if constexpr (is_ruvia_array<t_type> || is_ruvia_boxed_array<t_type>) {
            using element_t_type = typename t_type::value_type;
            if constexpr (is_model<element_t_type> || is_ruvia_array<element_t_type> || is_ruvia_boxed_array<element_t_type>) {
                for (const auto& element : value) {
                    if (!value_structure_valid(element)) {
                        return false;
                    }
                }
            }
            return true;
        } else {
            return true;
        }
    }

private:
    template <typename value_t_type, typename validator_t_type>
    static void validate_value_structure(
        const value_t_type& value, std::string_view path, validator_t_type& validator_value) {
        using t_type = std::remove_cvref_t<value_t_type>;
        if constexpr (is_model<t_type>) {
            validate_structure(value, path, validator_value);
        } else if constexpr (is_ruvia_array<t_type> || is_ruvia_boxed_array<t_type>) {
            using element_t_type = typename t_type::value_type;
            if constexpr (is_model<element_t_type> || is_ruvia_array<element_t_type> || is_ruvia_boxed_array<element_t_type>) {
                std::size_t index = 0;
                for (const auto& element : value) {
                    if (validator_value.full()) {
                        break;
                    }
                    std::pmr::string item_path(validator_value.resource());
                    model::append_index_path(item_path, path, index++);
                    validate_value_structure(element, item_path, validator_value);
                }
            }
        }
    }

    template <typename value_t_type, typename validator_t_type>
    static void validate_nested_field_rules(
        const value_t_type& value, std::string_view path, validator_t_type& validator_value) {
        using t_type = std::remove_cvref_t<value_t_type>;
        if constexpr (is_model<t_type>) {
            validate_field_rules(value, path, validator_value);
        } else if constexpr (is_ruvia_array<t_type> || is_ruvia_boxed_array<t_type>) {
            using element_t_type = typename t_type::value_type;
            if constexpr (is_model<element_t_type> || is_ruvia_array<element_t_type> || is_ruvia_boxed_array<element_t_type>) {
                std::size_t index = 0;
                for (const auto& element : value) {
                    if (validator_value.full()) {
                        break;
                    }
                    std::pmr::string item_path(validator_value.resource());
                    model::append_index_path(item_path, path, index++);
                    validate_nested_field_rules(element, item_path, validator_value);
                }
            }
        }
    }
};

}  // namespace ruvia::detail
