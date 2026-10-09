#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#include "ruvia/web/detail/model/model_field.h"
#include "ruvia/web/detail/model/rule/rule_pack.h"
#include "ruvia/web/detail/model/traits.h"

namespace ruvia::detail::model {

template <typename t_type>
using field_option_tuple_type =
    std::conditional_t<is_model_option<t_type>(), std::tuple<t_type>, std::tuple<>>;

template <typename t_type>
using field_rule_tuple_type =
    std::conditional_t<is_validation_rule<t_type>(), std::tuple<t_type>, std::tuple<>>;

template <typename tuple_type>
struct tuple_model_options;

template <typename... ts_type>
struct tuple_model_options<std::tuple<ts_type...>> {
    using type = model_options<ts_type...>;
};

template <typename tuple_type>
struct tuple_rules;

template <typename... ts_type>
struct tuple_rules<std::tuple<ts_type...>> {
    using type = rules<ts_type...>;
};

template <fixed_string field_source_name, fixed_string field_wire_name, typename value_t_type, bool is_required,
    typename... arg_ts_type>
struct model_field_descriptor final {
    static_assert(((is_model_option<arg_ts_type>() || is_validation_rule<arg_ts_type>()) && ... && true),
        "RUVIA_REQUIRED_FIELD/RUVIA_OPTIONAL_FIELD accept model options (RUVIA_DEFAULT, "
        "RUVIA_INITIAL, RUVIA_NULLABLE, RUVIA_OMIT_EMPTY, RUVIA_EMIT_NULL) and validation rules "
        "(RUVIA_MIN, RUVIA_EMAIL, ...)");

    using value_type = value_t_type;
    using options_type = typename tuple_model_options<decltype(std::tuple_cat(std::tuple<>{},
        std::declval<field_option_tuple_type<arg_ts_type>>()...))>::type;
    using rules_type = typename tuple_rules<decltype(std::tuple_cat(std::tuple<>{},
        std::declval<field_rule_tuple_type<arg_ts_type>>()...))>::type;
    using field_type = model_field<value_t_type, is_required, options_type, field_wire_name>;

    static constexpr auto source_name = field_source_name;
    static constexpr auto wire_name = field_wire_name;
    static constexpr auto wire_hash = model_field_name_hash(wire_name.view());
    static constexpr bool required = is_required;
    static constexpr bool nullable = options_type::nullable;
    static constexpr bool has_field_rules = !std::is_same_v<rules_type, rules<>>;

    static_assert(!has_field_rules ||
                      !(detail::is_ruvia_json_value<value_t_type> || detail::is_ruvia_json_object<value_t_type>),
        "json_value/json_object are dynamic content, not typed field validation schemas");
};

template <typename... descriptor_ts_type>
struct model_schema final {};

struct empty_model_tag final {};

struct model_access final {
    template <typename model_type>
    [[nodiscard]] static constexpr auto schema() noexcept {
        return typename std::remove_cvref_t<model_type>::ruvia_model_schema_type{};
    }

    template <std::size_t index, typename model_type>
    [[nodiscard]] static decltype(auto) slot(model_type& value) noexcept {
        return value.fields_.template slot<index>();
    }

    template <typename model_type>
    [[nodiscard]] static model_type empty(std::pmr::memory_resource* resource) {
        return model_type(empty_model_tag{}, ::ruvia::model_options{.resource_ = resource});
    }
};

template <typename... descriptor_types>
consteval void validate_model_schema(model_schema<descriptor_types...>) {
    static_assert((detail::is_model_field<typename descriptor_types::value_type> && ...),
        "model fields must use Ruvia values or nested models");
}

template <fixed_string field, typename... descriptor_ts_type>
[[nodiscard]] consteval std::size_t model_field_index() {
    constexpr std::size_t matches = (std::size_t{field == descriptor_ts_type::source_name} + ... + 0);
    static_assert(matches == 1, "unknown or duplicate Ruvia model field");

    constexpr std::array<bool, sizeof...(descriptor_ts_type)> fields_value{
        field == descriptor_ts_type::source_name...};
    for (std::size_t index = 0; index < fields_value.size(); ++index) {
        if (fields_value[index]) {
            return index;
        }
    }
    return 0;
}

template <typename... descriptor_ts_type>
[[nodiscard]] consteval bool unique_model_field_names() {
    constexpr std::array<std::string_view, sizeof...(descriptor_ts_type)> names{
        descriptor_ts_type::source_name.view()...};
    for (std::size_t left = 0; left < names.size(); ++left) {
        for (std::size_t right = left + 1; right < names.size(); ++right) {
            if (names[left] == names[right]) {
                return false;
            }
        }
    }
    return true;
}

template <typename... descriptor_ts_type>
[[nodiscard]] consteval bool unique_model_wire_names() {
    constexpr std::array<std::string_view, sizeof...(descriptor_ts_type)> names{
        descriptor_ts_type::wire_name.view()...};
    for (std::size_t left = 0; left < names.size(); ++left) {
        for (std::size_t right = left + 1; right < names.size(); ++right) {
            if (names[left] == names[right]) {
                return false;
            }
        }
    }
    return true;
}

template <typename model_t_type, typename... descriptor_ts_type, typename visitor_t_type, std::size_t... indices>
constexpr void visit_model_fields_impl(model_t_type& model, model_schema<descriptor_ts_type...>, visitor_t_type&& visitor,
    std::index_sequence<indices...>) {
    auto& visitor_ref = visitor;
    (visitor_ref(descriptor_ts_type{}, model_access::slot<indices>(model)), ...);
}

template <typename model_t_type, typename... descriptor_ts_type, typename visitor_t_type>
constexpr void visit_model_fields(
    model_t_type& model, model_schema<descriptor_ts_type...> schema, visitor_t_type&& visitor) {
    visit_model_fields_impl(
        model, schema, std::forward<visitor_t_type>(visitor), std::index_sequence_for<descriptor_ts_type...>{});
}

template <std::size_t index, typename descriptor_t_type, typename... remaining_ts_type, typename model_t_type,
    typename visitor_t_type>
[[nodiscard]] bool visit_model_field_by_wire_name_impl(model_t_type& model, std::uint64_t wire_hash,
    std::string_view wire_name, bool& visit_result, visitor_t_type& visitor) {
    if (wire_hash == descriptor_t_type::wire_hash && wire_name == descriptor_t_type::wire_name.view()) {
        visit_result = visitor(model_access::slot<index>(model));
        return true;
    }
    if constexpr (sizeof...(remaining_ts_type) > 0) {
        return visit_model_field_by_wire_name_impl<index + 1, remaining_ts_type...>(
            model, wire_hash, wire_name, visit_result, visitor);
    } else {
        return false;
    }
}

template <typename model_t_type, typename... descriptor_ts_type, typename visitor_t_type>
[[nodiscard]] bool visit_model_field_by_wire_name(model_t_type& model, model_schema<descriptor_ts_type...>,
    std::uint64_t wire_hash, std::string_view wire_name, bool& visit_result, visitor_t_type&& visitor) {
    if constexpr (sizeof...(descriptor_ts_type) == 0) {
        return false;
    } else {
        auto& visitor_ref = visitor;
        return visit_model_field_by_wire_name_impl<0, descriptor_ts_type...>(
            model, wire_hash, wire_name, visit_result, visitor_ref);
    }
}

template <fixed_string field, typename model_t_type, typename... descriptor_ts_type>
[[nodiscard]] model_field_state model_field_state(const model_t_type& model, model_schema<descriptor_ts_type...>) {
    constexpr auto index = model_field_index<field, descriptor_ts_type...>();
    return model_access::slot<index>(model).state();
}

template <typename model_type>
void initialize_model(model_type& value) {
    visit_model_fields(value, model_access::schema<model_type>(),
        [resource = value.resource()](const auto&, auto& slot) { slot.apply_initial(resource); });
}

}  // namespace ruvia::detail::model
