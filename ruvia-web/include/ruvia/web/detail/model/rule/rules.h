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
    template <typename model_type>
    [[nodiscard]] static bool structure_valid(const model_type& model) {
        bool valid = true;
        model::visit_model_fields(model, model::model_access::schema<model_type>(), [&](const auto&, const auto& slot) {
            using slot_type = std::remove_cvref_t<decltype(slot)>;
            const auto state_value = slot.state();
            if (state_value == model_field_state::duplicate || state_value == model_field_state::invalid_type ||
                (slot_type::required && state_value == model_field_state::missing)) {
                valid = false;
                return;
            }
            if (const auto& value = slot.value(); value && !value_structure_valid(*value)) {
                valid = false;
            }
        });
        return valid;
    }

    template <typename model_type, typename validator_type>
    static void validate_model(const model_type& model_value, validator_type& validator_value) {
        model::model_issue_sink sink(validator_value);
        const model::model_path root;
        validate_structure(model_value, root, sink);
        validate_field_rules(model_value, root, sink);
    }

    template <typename value_type>
    [[nodiscard]] static consteval bool contains_model() noexcept {
        using t_type = std::remove_cvref_t<value_type>;
        if constexpr (is_model<t_type>) {
            return true;
        } else if constexpr (is_ruvia_array<t_type> || is_ruvia_boxed_array<t_type>) {
            return contains_model<typename t_type::value_type>();
        } else {
            return false;
        }
    }

    template <typename value_type, typename validator_type>
    static void validate_value(const value_type& value, validator_type& validator_value) {
        model::model_issue_sink sink(validator_value);
        const model::model_path root;
        validate_value_structure(value, root, sink);
        validate_nested_field_rules(value, root, sink);
    }
    template <typename value_type>
    [[nodiscard]] static bool value_structure_valid(const value_type& value) {
        using t_type = std::remove_cvref_t<value_type>;
        if constexpr (is_model<t_type>) {
            return structure_valid(value);
        } else if constexpr (is_ruvia_array<t_type> || is_ruvia_boxed_array<t_type>) {
            using element_type = typename t_type::value_type;
            if constexpr (is_model<element_type> || is_ruvia_array<element_type> || is_ruvia_boxed_array<element_type>) {
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
    template <typename model_type, typename sink_type>
    static void validate_structure(
        const model_type& model_value, const model::model_path& prefix, sink_type& sink) {
        model::visit_model_fields(
            model_value, model::model_access::schema<model_type>(), [&](const auto&, const auto& slot) {
                if (sink.full()) {
                    return;
                }
                using slot_type = std::remove_cvref_t<decltype(slot)>;
                const model::model_path path(prefix, slot.wire_name());

                switch (slot.state()) {
                    case model_field_state::duplicate:
                        sink.add(path, "duplicate", "is duplicated");
                        return;
                    case model_field_state::invalid_type:
                        sink.add(path, "invalid_type", model::expected_type_name<typename slot_type::value_type>());
                        return;
                    case model_field_state::missing:
                        if constexpr (slot_type::required) {
                            sink.add(path, "required", "is required");
                        }
                        return;
                    case model_field_state::null:
                        return;
                    case model_field_state::parsed:
                        break;
                }

                validate_value_structure(*slot.value(), path, sink);
            });
    }

    template <typename model_type, typename sink_type>
    static void validate_field_rules(
        const model_type& model_value, const model::model_path& prefix, sink_type& sink) {
        model::visit_model_fields(
            model_value, model::model_access::schema<model_type>(), [&](const auto& descriptor, const auto& slot) {
                if (sink.full()) {
                    return;
                }
                using descriptor_type = std::remove_cvref_t<decltype(descriptor)>;
                const model::model_path path(prefix, slot.wire_name());
                typename descriptor_type::rules_type{}.validate(slot.state(), slot.value(), path, sink);

                if (slot.state() != model_field_state::parsed || !slot.value()) {
                    return;
                }
                validate_nested_field_rules(*slot.value(), path, sink);
            });
    }

    template <typename value_type, typename sink_type>
    static void validate_value_structure(
        const value_type& value, const model::model_path& path, sink_type& sink) {
        using t_type = std::remove_cvref_t<value_type>;
        if constexpr (is_model<t_type>) {
            validate_structure(value, path, sink);
        } else if constexpr (is_ruvia_array<t_type> || is_ruvia_boxed_array<t_type>) {
            using element_type = typename t_type::value_type;
            if constexpr (is_model<element_type> || is_ruvia_array<element_type> || is_ruvia_boxed_array<element_type>) {
                std::size_t index = 0;
                for (const auto& element : value) {
                    if (sink.full()) {
                        break;
                    }
                    const model::model_path item_path(path, index++);
                    validate_value_structure(element, item_path, sink);
                }
            }
        }
    }

    template <typename value_type, typename sink_type>
    static void validate_nested_field_rules(
        const value_type& value, const model::model_path& path, sink_type& sink) {
        using t_type = std::remove_cvref_t<value_type>;
        if constexpr (is_model<t_type>) {
            validate_field_rules(value, path, sink);
        } else if constexpr (is_ruvia_array<t_type> || is_ruvia_boxed_array<t_type>) {
            using element_type = typename t_type::value_type;
            if constexpr (is_model<element_type> || is_ruvia_array<element_type> || is_ruvia_boxed_array<element_type>) {
                std::size_t index = 0;
                for (const auto& element : value) {
                    if (sink.full()) {
                        break;
                    }
                    const model::model_path item_path(path, index++);
                    validate_nested_field_rules(element, item_path, sink);
                }
            }
        }
    }
};

}  // namespace ruvia::detail
