#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/web/db/db_query.h"
#include "ruvia/web/detail/entity/value_storage.h"
#include "ruvia/web/entity_rows.h"
#include "ruvia/web/model.h"

namespace ruvia {

namespace detail {
template <typename e_type>
struct db_entity_access;
}  // namespace detail

enum class db_relation_kind : unsigned char { one_to_one,
    many_to_one,
    one_to_many,
    many_to_many };

template <fixed_string local_name, fixed_string referenced_name>
struct db_join_column final {
    static constexpr auto local = local_name;
    static constexpr auto referenced = referenced_name;
};

template <typename... j_type>
struct db_join_columns final {
    using tuple_type = std::tuple<j_type...>;
};
template <fixed_string field_name>
struct db_inverse final {
    static constexpr auto name = field_name;
};
namespace detail {
template <typename t_type>
struct is_db_inverse : std::false_type {};
template <fixed_string n>
struct is_db_inverse<db_inverse<n>> : std::true_type {};
template <typename t_type>
struct db_inverse_name {
    static constexpr auto value = fixed_string{""};
};
template <fixed_string n>
struct db_inverse_name<db_inverse<n>> {
    static constexpr auto value = n;
};
template <typename... t_type>
struct relation_join_tuple {
    using type = std::tuple<t_type...>;
};
template <typename t_type>
    requires requires { typename t_type::tuple_type; }
struct relation_join_tuple<t_type> {
    using type = typename t_type::tuple_type;
};
template <typename... t_type>
struct first_inverse_name {
    static constexpr auto value = fixed_string{""};
};
template <typename t_type, typename... rest_type>
struct first_inverse_name<t_type, rest_type...> {
    static constexpr auto value = [] {
        if constexpr (is_db_inverse<t_type>::value) {
            return db_inverse_name<t_type>::value;
        } else {
            return first_inverse_name<rest_type...>::value;
        }
    }();
};
}  // namespace detail
template <fixed_string table, typename owner, typename inverse_type>
struct db_join_table final {
    static constexpr auto name = table;
    using owner_columns_type = typename owner::tuple_type;
    using inverse_columns_type = typename inverse_type::tuple_type;
};
namespace detail {
template <typename t_type>
struct is_db_join_table : std::false_type {};
template <fixed_string n, typename o_type, typename i_type>
struct is_db_join_table<db_join_table<n, o_type, i_type>> : std::true_type {};
}  // namespace detail

template <fixed_string field_name, typename target_type, typename... js_type>
struct db_many_to_one final {
    static constexpr auto name = field_name;
    using target_entity_type = target_type;
    using join_columns_type = std::tuple<js_type...>;
    static constexpr auto kind = db_relation_kind::many_to_one;
    static constexpr bool is_collection = false;
    static constexpr bool is_owning = true;
    static constexpr auto inverse_name = fixed_string{""};
};
template <fixed_string field_name, typename target_type, typename... mapping_type>
struct db_one_to_one final {
    static constexpr auto name = field_name;
    using target_entity_type = target_type;
    static constexpr auto kind = db_relation_kind::one_to_one;
    static constexpr bool is_collection = false;
    static constexpr bool is_owning = !(sizeof...(mapping_type) == 1 && (detail::is_db_inverse<mapping_type>::value && ...));
    using join_columns_type = typename detail::relation_join_tuple<mapping_type...>::type;
    static constexpr auto inverse_name = detail::first_inverse_name<mapping_type...>::value;
};
template <fixed_string field_name, typename target_type, fixed_string inverse>
struct db_one_to_many final {
    static constexpr auto name = field_name;
    using target_entity_type = target_type;
    static constexpr auto kind = db_relation_kind::one_to_many;
    static constexpr bool is_collection = true;
    static constexpr bool is_owning = false;
    static constexpr auto inverse_name = inverse;
    using join_columns_type = std::tuple<>;
};
template <fixed_string field_name, typename target_type, typename mapping_type>
struct db_many_to_many final {
    static constexpr auto name = field_name;
    using target_entity_type = target_type;
    using join_table_type = mapping_type;
    static constexpr auto kind = db_relation_kind::many_to_many;
    static constexpr bool is_collection = true;
    static constexpr bool is_owning = detail::is_db_join_table<mapping_type>::value;
    static constexpr auto inverse_name = [] {
        if constexpr (detail::is_db_inverse<mapping_type>::value) {
            return mapping_type::name;
        } else {
            return fixed_string{""};
        }
    }();
    using join_columns_type = std::tuple<>;
};

enum class db_generated_type : unsigned char { none,
    stored,
    virtual_value };

template <typename enum_name_type = fixed_string<1>, typename default_expression_type = fixed_string<1>>
struct db_column_options final {
    db_data_type data_type_{db_data_type::inferred};
    bool primary_key_{false};
    bool generated_{false};
    db_generated_type generated_type_{db_generated_type::none};
    bool nullable_{false};
    bool read_only_{false};
    bool create_date_{false};
    bool update_date_{false};
    bool delete_date_{false};
    bool version_{false};
    std::size_t length_{0};
    unsigned precision_{0};
    unsigned scale_{0};
    enum_name_type enum_name_{""};
    default_expression_type default_expression_{""};
    constexpr bool operator==(const db_column_options&) const = default;
};

namespace detail {

template <typename t_type>
struct db_entity_type_traits {
    using value_type = t_type;
    static constexpr db_data_type data_type = db_data_type::inferred;
};
template <>
struct db_entity_type_traits<bool> {
    using value_type = bool;
    static constexpr db_data_type data_type = db_data_type::boolean;
};
template <>
struct db_entity_type_traits<std::int16_t> {
    using value_type = std::int16_t;
    static constexpr db_data_type data_type = db_data_type::small_int;
};
template <>
struct db_entity_type_traits<std::int32_t> {
    using value_type = std::int32_t;
    static constexpr db_data_type data_type = db_data_type::integer;
};
template <>
struct db_entity_type_traits<std::int64_t> {
    using value_type = std::int64_t;
    static constexpr db_data_type data_type = db_data_type::big_int;
};
template <>
struct db_entity_type_traits<float> {
    using value_type = float;
    static constexpr db_data_type data_type = db_data_type::real;
};
template <>
struct db_entity_type_traits<double> {
    using value_type = double;
    static constexpr db_data_type data_type = db_data_type::double_value;
};
template <>
struct db_entity_type_traits<std::pmr::string> {
    using value_type = std::pmr::string;
    static constexpr db_data_type data_type = db_data_type::text;
};
template <>
struct db_entity_type_traits<string> {
    using value_type = string;
    static constexpr db_data_type data_type = db_data_type::text;
};
template <typename t_type>
struct db_entity_type_traits<std::optional<t_type>> {
    using value_type = typename db_entity_type_traits<t_type>::value_type;
    static constexpr db_data_type data_type = db_entity_type_traits<t_type>::data_type;
};
template <typename t_type>
struct db_entity_type_traits<std::pmr::vector<t_type>> {
    using value_type = std::pmr::vector<t_type>;
    static constexpr db_data_type data_type = db_data_type::array;
};

// Column descriptor predicates shared by typed field references and the
// repository builders so both agree on which columns accept writes and
// arithmetic updates.
template <typename c_type>
consteval bool is_writable_db_column() noexcept {
    return c_type::options.generated_type_ == db_generated_type::none && !c_type::options.read_only_;
}
template <typename c_type>
consteval bool is_numeric_db_column() noexcept {
    return !is_pmr_vector<typename c_type::value_type>::value &&
           (c_type::data_type == db_data_type::small_int ||
               c_type::data_type == db_data_type::integer ||
               c_type::data_type == db_data_type::big_int ||
               c_type::data_type == db_data_type::numeric ||
               c_type::data_type == db_data_type::real ||
               c_type::data_type == db_data_type::double_value);
}

template <typename t_type>
struct db_relation_deleter final {
    std::pmr::memory_resource* resource_{nullptr};
    void operator()(t_type* value) const noexcept {
        if (value != nullptr) {
            std::pmr::polymorphic_allocator<t_type> allocator(resource_);
            allocator.delete_object(value);
        }
    }
};

template <typename t_type, bool collection = false>
struct db_relation_slot final {
    enum class state_type : unsigned char { unset,
        null,
        value };
    using stored_type = std::conditional_t<collection, entity_rows<t_type>, t_type>;
    using pointer = std::unique_ptr<stored_type, db_relation_deleter<stored_type>>;
    explicit db_relation_slot(std::pmr::memory_resource* resource)
        : resource_(resource),
          value_(nullptr, db_relation_deleter<stored_type>{resource}) {}
    db_relation_slot(const db_relation_slot&) = delete;
    db_relation_slot& operator=(const db_relation_slot&) = delete;
    db_relation_slot(db_relation_slot&& other) noexcept
        : resource_(other.resource_),
          state_(std::exchange(other.state_, state_type::unset)),
          value_(std::move(other.value_)) {}
    db_relation_slot& operator=(db_relation_slot&&) = delete;
    std::pmr::memory_resource* resource_;
    state_type state_{state_type::unset};
    pointer value_;
    void reset() noexcept {
        value_.reset();
        state_ = state_type::unset;
    }
};

template <template <typename> class predicate_type, typename... ts_type>
struct tuple_filter;
template <template <typename> class predicate_type>
struct tuple_filter<predicate_type> {
    using type = std::tuple<>;
};
template <template <typename> class predicate_type, typename t_type, typename... ts_type>
struct tuple_filter<predicate_type, t_type, ts_type...> {
    using tail = typename tuple_filter<predicate_type, ts_type...>::type;
    using type = std::conditional_t<predicate_type<t_type>::value, decltype(std::tuple_cat(std::declval<std::tuple<t_type>>(), std::declval<tail>())), tail>;
};
template <template <typename> class predicate_type, typename... ts_type>
using tuple_filter_t = typename tuple_filter<predicate_type, ts_type...>::type;

}  // namespace detail

// PostgreSQL array columns use std::pmr::vector of scalar or optional elements.
// Decoding retains one-dimensional element order and discards the lower bound;
// multidimensional arrays are rejected with db_conversion_error.
template <fixed_string field_name, typename t_type, db_column_options options_type = db_column_options{}>
struct db_column final {
    static_assert(!detail::is_optional<t_type>::value, "use nullable column options and set_null for entity fields; optional is only for array elements");
    static constexpr auto name = field_name;
    using value_type = t_type;
    static consteval auto normalize_options(auto opts) noexcept {
        if (opts.delete_date_) {
            opts.nullable_ = true;
        }
        return opts;
    }
    static constexpr auto options = normalize_options(options_type);
    static_assert(!std::is_same_v<t_type, char> && !std::is_same_v<t_type, std::string_view>,
        "database entity columns must own their values");
    static constexpr db_data_type data_type = options_type.data_type_ == db_data_type::inferred
                                                  ? detail::db_entity_type_traits<t_type>::data_type
                                                  : options_type.data_type_;
};

template <typename t_type>
struct is_db_column : std::false_type {};
template <fixed_string name, typename t_type, db_column_options options_type>
struct is_db_column<db_column<name, t_type, options_type>> : std::true_type {};
template <typename t_type>
struct is_db_relation : std::false_type {};
template <fixed_string name, typename target_type, typename... joins_type>
struct is_db_relation<db_many_to_one<name, target_type, joins_type...>> : std::true_type {};
template <fixed_string name, typename target_type, typename... mapping_type>
struct is_db_relation<db_one_to_one<name, target_type, mapping_type...>> : std::true_type {};
template <fixed_string name, typename target_type, fixed_string inverse>
struct is_db_relation<db_one_to_many<name, target_type, inverse>> : std::true_type {};
template <fixed_string name, typename target_type, typename mapping_type>
struct is_db_relation<db_many_to_many<name, target_type, mapping_type>> : std::true_type {};

namespace detail {

template <fixed_string table, typename... members_type>
class db_entity_storage final {
    static_assert(((is_db_column<members_type>::value || is_db_relation<members_type>::value) && ...),
        "SQL entities require RUVIA_DB_COLUMN or SQL relation descriptors");
    using columns_tuple_type = tuple_filter_t<is_db_column, members_type...>;
    using relations_tuple_type = tuple_filter_t<is_db_relation, members_type...>;
    static_assert(unique_entity_columns<members_type...>(), "duplicate database entity member name");
    static consteval std::size_t count_lifecycle_columns(auto predicate) noexcept {
        return [&]<typename... cols>(std::tuple<cols...>*) {
            return (std::size_t{predicate(cols::options)} + ... + std::size_t{0});
        }(static_cast<columns_tuple_type*>(nullptr));
    }
    static_assert(count_lifecycle_columns([](const auto& opts) { return opts.create_date_; }) <= 1,
        "database entity may declare at most one create_date_ column");
    static_assert(count_lifecycle_columns([](const auto& opts) { return opts.update_date_; }) <= 1,
        "database entity may declare at most one update_date_ column");
    static_assert(count_lifecycle_columns([](const auto& opts) { return opts.delete_date_; }) <= 1,
        "database entity may declare at most one delete_date_ column");
    static_assert(count_lifecycle_columns([](const auto& opts) { return opts.version_; }) <= 1,
        "database entity may declare at most one version_ column");
    template <typename r_type>
    using relation_slot_type = db_relation_slot<typename r_type::target_entity_type, r_type::is_collection>;
    template <std::size_t... indexes>
    static auto make_relation_slots(std::pmr::memory_resource* resource, std::index_sequence<indexes...>) {
        return std::tuple<relation_slot_type<std::tuple_element_t<indexes, relations_tuple_type>>...>(relation_slot_type<std::tuple_element_t<indexes, relations_tuple_type>>(resource)...);
    }
    using column_storage = typename entity_storage_from_tuple<columns_tuple_type>::type;
    using relation_slots_type = decltype(make_relation_slots(nullptr, std::make_index_sequence<std::tuple_size_v<relations_tuple_type>>{}));
    template <fixed_string name>
    static consteval std::size_t index() {
        return column_storage::template index<name>();
    }
    template <fixed_string name>
    static consteval std::size_t relation_index() {
        constexpr auto result_value = []<std::size_t... indexes>(std::index_sequence<indexes...>) consteval {
            constexpr bool matches[] = {std::tuple_element_t<indexes, relations_tuple_type>::name == name...};
            for (std::size_t index = 0; index < sizeof...(indexes); ++index) {
                if (matches[index]) {
                    return index;
                }
            }
            return sizeof...(indexes);
        }(std::make_index_sequence<std::tuple_size_v<relations_tuple_type>>{});
        static_assert(result_value < std::tuple_size_v<relations_tuple_type>, "unknown database entity relation");
        return result_value;
    }
    template <fixed_string name>
    static consteval bool is_relation() {
        constexpr bool matches[] = {members_type::name == name...};
        constexpr bool columns[] = {is_db_column<members_type>::value...};
        for (std::size_t i = 0; i < sizeof...(members_type); ++i) {
            if (matches[i]) {
                return !columns[i];
            }
        }
        return false;
    }
    template <fixed_string name>
    auto& relation_slot() {
        return std::get<relation_index<name>()>(relation_slots_);
    }
    template <fixed_string name>
    const auto& relation_slot() const {
        return std::get<relation_index<name>()>(relation_slots_);
    }

public:
    using columns_type = columns_tuple_type;
    using relations_type = relations_tuple_type;
    static constexpr std::string_view table_name() noexcept {
        return table.view();
    }
    template <fixed_string name>
    static consteval std::size_t column_index() {
        return index<name>();
    }
    template <fixed_string name>
    static consteval std::string_view column_name() noexcept {
        return std::tuple_element_t<index<name>(), columns_type>::name.view();
    }
    explicit db_entity_storage(std::pmr::memory_resource* resource = nullptr)
        : columns_(resource),
          relation_slots_(make_relation_slots(columns_.resource(), std::make_index_sequence<std::tuple_size_v<relations_tuple_type>>{})) {}
    template <typename owner_type>
        requires requires(owner_type& owner) {
            { owner.resource() } -> std::convertible_to<std::pmr::memory_resource*>;
        }
    explicit db_entity_storage(owner_type& owner)
        : db_entity_storage(owner.resource()) {}
    db_entity_storage(const db_entity_storage&) = delete;
    db_entity_storage& operator=(const db_entity_storage&) = delete;
    db_entity_storage(db_entity_storage&&) noexcept = default;
    template <fixed_string name>
    auto& get() & {
        if constexpr (is_relation<name>()) {
            auto& s = relation_slot<name>();
            if (s.state_ != decltype(s.state_)::value) {
                throw std::logic_error("database entity relation is not loaded");
            }
            return *s.value_;
        } else {
            return columns_.template get<name>();
        }
    }
    template <fixed_string name>
    const auto& get() const& {
        return const_cast<db_entity_storage*>(this)->get<name>();
    }
    template <fixed_string name>
    const auto& get() const&& = delete;
    template <fixed_string name, typename v_type>
    void set(v_type&& value) {
        static_assert(!is_relation<name>(), "relations cannot be assigned; use relation loading access");
        columns_.template set<name>(std::forward<v_type>(value));
    }
    template <fixed_string name>
    void set_null()
        requires(!is_relation<name>() && std::tuple_element_t<index<name>(), columns_type>::options.nullable_)
    {
        columns_.template set_null<name>();
    }
    template <fixed_string name>
    void reset() {
        if constexpr (is_relation<name>()) {
            relation_slot<name>().reset();
        } else {
            columns_.template reset<name>();
        }
    }
    template <fixed_string name>
    bool is_set() const {
        if constexpr (is_relation<name>()) {
            return relation_slot<name>().state_ != decltype(relation_slot<name>().state_)::unset;
        } else {
            return columns_.template is_set<name>();
        }
    }
    template <fixed_string name>
    bool is_null() const {
        if constexpr (is_relation<name>()) {
            return relation_slot<name>().state_ == decltype(relation_slot<name>().state_)::null;
        } else {
            return columns_.template is_null<name>();
        }
    }
    std::pmr::memory_resource* resource() const noexcept {
        return columns_.resource();
    }

private:
    template <typename>
    friend struct db_entity_access;
    column_storage columns_;
    relation_slots_type relation_slots_;
};

template <typename columns>
inline constexpr bool sql_columns = []<std::size_t... indices>(std::index_sequence<indices...>) {
    return (is_db_column<std::tuple_element_t<indices, columns>>::value && ...);
}(std::make_index_sequence<std::tuple_size_v<columns>>{});

}  // namespace detail

template <typename entity>
concept sql_entity = requires(const entity& value) {
    typename entity::columns_type;
    typename entity::relations_type;
    typename entity::sql_entity_type;
    requires std::same_as<entity, typename entity::sql_entity_type>;
    requires detail::sql_columns<typename entity::columns_type>;
    { entity::table_name() } -> std::convertible_to<std::string_view>;
    { value.resource() } -> std::same_as<std::pmr::memory_resource*>;
};

template <typename projection>
concept sql_projection = requires(const projection& value) {
    typename projection::columns_type;
    typename projection::db_projection_type;
    requires std::same_as<projection, typename projection::db_projection_type>;
    requires detail::sql_columns<typename projection::columns_type>;
    { value.resource() } -> std::same_as<std::pmr::memory_resource*>;
};

template <typename e_type, fixed_string field_name>
class db_field_reference final {
    static_assert(sql_entity<e_type>, "SQL field references require a SQL entity");
    static_assert(e_type::template column_index<field_name>() < std::tuple_size_v<typename e_type::columns_type>);
    using column_type = std::tuple_element_t<e_type::template column_index<field_name>(), typename e_type::columns_type>;
    static constexpr bool is_writable = detail::is_writable_db_column<column_type>();
    static constexpr bool is_numeric = detail::is_numeric_db_column<column_type>();

public:
    constexpr db_field_reference(std::string_view alias = {}) noexcept
        : alias_(alias) {}
    constexpr db_field_reference(const db_field_reference&) noexcept = default;
    constexpr db_field_reference(db_field_reference&&) noexcept = default;
    constexpr db_field_reference& operator=(const db_field_reference&) noexcept = default;
    constexpr db_field_reference& operator=(db_field_reference&&) noexcept = default;

    [[nodiscard]] static constexpr std::string_view name() noexcept {
        return field_name.view();
    }
    [[nodiscard]] static constexpr std::string_view table_name() noexcept {
        return e_type::table_name();
    }
    [[nodiscard]] constexpr std::string_view alias() const noexcept {
        return alias_;
    }
    [[nodiscard]] constexpr db_field_reference with_alias(std::string_view alias) const noexcept {
        return db_field_reference(alias);
    }
    [[nodiscard]] db_expression expression(db_query& query, std::string_view fallback_alias = {}) const {
        const auto qualifier = !alias_.empty() ? alias_ : (fallback_alias.empty() ? e_type::table_name() : fallback_alias);
        return query.column(field_name.view(), qualifier);
    }
    [[nodiscard]] db_expression excluded(db_query& query) const {
        return query.excluded(field_name.view());
    }
    [[nodiscard]] db_expr to_expr() const {
        return db_expr(detail::db_owned_expression::make_static_column(
            field_name.view(), e_type::table_name(), alias_));
    }
    [[nodiscard]] operator db_expr() const {
        return to_expr();
    }
    [[nodiscard]] db_expr excluded_expr() const noexcept {
        return db_expr(detail::db_owned_expression::make_static_excluded(
            field_name.view(), e_type::table_name()));
    }

    template <detail::db_parameter v_type>
    [[nodiscard]] db_predicate is_distinct_from(v_type&& value) const {
        return to_expr().is_distinct_from(std::forward<v_type>(value));
    }
    template <detail::db_parameter v_type>
    [[nodiscard]] db_predicate is_not_distinct_from(v_type&& value) const {
        return to_expr().is_not_distinct_from(std::forward<v_type>(value));
    }
    [[nodiscard]] db_predicate is_distinct_from(const db_expr& rhs) const {
        return to_expr().is_distinct_from(rhs);
    }
    [[nodiscard]] db_predicate is_not_distinct_from(const db_expr& rhs) const {
        return to_expr().is_not_distinct_from(rhs);
    }

    [[nodiscard]] db_predicate like(std::string_view pattern) const {
        return to_expr().like(pattern);
    }
    [[nodiscard]] db_predicate not_like(std::string_view pattern) const {
        return to_expr().not_like(pattern);
    }
    [[nodiscard]] db_predicate ilike(std::string_view pattern) const {
        return to_expr().ilike(pattern);
    }
    [[nodiscard]] db_predicate not_ilike(std::string_view pattern) const {
        return to_expr().not_ilike(pattern);
    }
    [[nodiscard]] db_predicate regex(std::string_view pattern) const {
        return to_expr().regex(pattern);
    }
    [[nodiscard]] db_predicate iregex(std::string_view pattern) const {
        return to_expr().iregex(pattern);
    }
    template <detail::db_parameter lower_type, detail::db_parameter upper_type>
    [[nodiscard]] db_predicate between(lower_type&& lower, upper_type&& upper) const {
        return to_expr().between(std::forward<lower_type>(lower), std::forward<upper_type>(upper));
    }
    template <detail::db_parameter lower_type, detail::db_parameter upper_type>
    [[nodiscard]] db_predicate not_between(lower_type&& lower, upper_type&& upper) const {
        return to_expr().not_between(std::forward<lower_type>(lower), std::forward<upper_type>(upper));
    }
    [[nodiscard]] db_predicate is_null() const {
        return to_expr().is_null();
    }
    [[nodiscard]] db_predicate is_not_null() const {
        return to_expr().is_not_null();
    }
    [[nodiscard]] db_predicate is_true() const {
        return to_expr().is_true();
    }
    [[nodiscard]] db_predicate is_false() const {
        return to_expr().is_false();
    }
    [[nodiscard]] db_predicate is_not_true() const {
        return to_expr().is_not_true();
    }
    [[nodiscard]] db_predicate is_not_false() const {
        return to_expr().is_not_false();
    }
    template <typename t_type>
    [[nodiscard]] db_predicate in(std::span<const t_type> values) const {
        return compare_list(db_binary_operator::in, values);
    }
    template <typename t_type>
    [[nodiscard]] db_predicate in(std::initializer_list<t_type> values) const {
        return in(std::span<const t_type>(values.begin(), values.size()));
    }
    template <typename t_type>
    [[nodiscard]] db_predicate not_in(std::span<const t_type> values) const {
        return compare_list(db_binary_operator::not_in, values);
    }
    template <typename t_type>
    [[nodiscard]] db_predicate not_in(std::initializer_list<t_type> values) const {
        return not_in(std::span<const t_type>(values.begin(), values.size()));
    }
    [[nodiscard]] db_predicate in(const db_query& subquery) const {
        return compare_subquery(db_binary_operator::in, subquery);
    }
    [[nodiscard]] db_predicate not_in(const db_query& subquery) const {
        return compare_subquery(db_binary_operator::not_in, subquery);
    }
    template <typename subquery_type>
        requires requires(const subquery_type& s) {
            { s.query() } -> std::same_as<const db_query&>;
        }
    [[nodiscard]] db_predicate in(const subquery_type& subquery) const {
        return compare_subquery(db_binary_operator::in, subquery.query());
    }
    template <typename subquery_type>
        requires requires(const subquery_type& s) {
            { s.query() } -> std::same_as<const db_query&>;
        }
    [[nodiscard]] db_predicate not_in(const subquery_type& subquery) const {
        return compare_subquery(db_binary_operator::not_in, subquery.query());
    }

    template <typename t_type>
    [[nodiscard]] db_predicate array_contains(std::span<const t_type> values) const {
        return compare_array(db_binary_operator::array_contains, values);
    }
    template <typename t_type>
    [[nodiscard]] db_predicate array_contains(std::initializer_list<t_type> values) const {
        return array_contains(std::span<const t_type>(values.begin(), values.size()));
    }
    template <typename t_type>
    [[nodiscard]] db_predicate array_contained_by(std::span<const t_type> values) const {
        return compare_array(db_binary_operator::array_contained_by, values);
    }
    template <typename t_type>
    [[nodiscard]] db_predicate array_contained_by(std::initializer_list<t_type> values) const {
        return array_contained_by(std::span<const t_type>(values.begin(), values.size()));
    }
    template <typename t_type>
    [[nodiscard]] db_predicate array_overlap(std::span<const t_type> values) const {
        return compare_array(db_binary_operator::array_overlap, values);
    }
    template <typename t_type>
    [[nodiscard]] db_predicate array_overlap(std::initializer_list<t_type> values) const {
        return array_overlap(std::span<const t_type>(values.begin(), values.size()));
    }

    [[nodiscard]] db_predicate json_contains(std::string_view json_text) const {
        return compare_json(db_binary_operator::json_contains, json_text);
    }
    [[nodiscard]] db_predicate json_contained_by(std::string_view json_text) const {
        return compare_json(db_binary_operator::json_contained_by, json_text);
    }
    [[nodiscard]] db_predicate json_has_key(std::string_view key) const {
        return to_expr().compare_expr(db_binary_operator::json_has_key, db_expr::value(key));
    }
    [[nodiscard]] db_predicate json_has_any_key(std::span<const std::string_view> keys) const {
        return compare_json_keys(db_binary_operator::json_has_any_key, keys);
    }
    [[nodiscard]] db_predicate json_has_any_key(std::initializer_list<std::string_view> keys) const {
        return json_has_any_key(std::span<const std::string_view>(keys.begin(), keys.size()));
    }
    [[nodiscard]] db_predicate json_has_all_keys(std::span<const std::string_view> keys) const {
        return compare_json_keys(db_binary_operator::json_has_all_keys, keys);
    }
    [[nodiscard]] db_predicate json_has_all_keys(std::initializer_list<std::string_view> keys) const {
        return json_has_all_keys(std::span<const std::string_view>(keys.begin(), keys.size()));
    }
    [[nodiscard]] db_predicate json_text_equals(std::string_view key, std::string_view value) const {
        return to_expr().json_get_text(key) == value;
    }
    [[nodiscard]] db_predicate json_path_text_equals(std::span<const std::string_view> path, std::string_view value) const {
        return to_expr().json_path_text(path) == value;
    }
    [[nodiscard]] db_predicate json_path_text_equals(std::initializer_list<std::string_view> path, std::string_view value) const {
        return json_path_text_equals(std::span<const std::string_view>(path.begin(), path.size()), value);
    }

    [[nodiscard]] db_selection select() const {
        return as(field_name.view());
    }
    [[nodiscard]] db_selection as(borrowed_text alias_name) const {
        return to_expr().as(alias_name);
    }
    template <detail::db_parameter v_type>
    [[nodiscard]] db_selection coalesce(v_type&& fallback, borrowed_text alias_name = field_name.view()) const {
        return db_expr::coalesce({to_expr(), db_expr::value(std::forward<v_type>(fallback))}).as(alias_name);
    }
    [[nodiscard]] db_selection coalesce_text(std::string_view fallback = {}, borrowed_text alias_name = field_name.view()) const {
        auto col_expr = to_expr();
        if constexpr (column_type::data_type != db_data_type::text &&
                      column_type::data_type != db_data_type::varchar &&
                      column_type::data_type != db_data_type::char_value) {
            col_expr = col_expr.cast(db_data_type::text);
        }
        return db_expr::coalesce({col_expr, db_expr::value(fallback)}).as(alias_name);
    }

    template <detail::db_parameter v_type>
        requires(is_writable && (!std::same_as<std::remove_cvref_t<v_type>, std::nullptr_t> || column_type::options.nullable_))
    [[nodiscard]] db_assignment set(v_type&& value) const {
        return db_assignment{field_name.view(), db_expr::value(std::forward<v_type>(value))};
    }
    template <detail::db_parameter v_type>
        requires(is_writable && (!std::same_as<std::remove_cvref_t<v_type>, std::nullptr_t> || column_type::options.nullable_))
    [[nodiscard]] db_assignment operator=(v_type&& value) const {
        return set(std::forward<v_type>(value));
    }
    [[nodiscard]] db_assignment set(const db_expr& value) const
        requires is_writable
    {
        if (value.empty()) {
            throw std::invalid_argument("cannot assign an empty database expression");
        }
        return db_assignment{field_name.view(), value};
    }
    [[nodiscard]] db_assignment operator=(const db_expr& value) const
        requires is_writable
    {
        return set(value);
    }
    [[nodiscard]] db_assignment set_null() const
        requires(is_writable && column_type::options.nullable_)
    {
        return db_assignment{field_name.view(), db_expr::null()};
    }
    [[nodiscard]] db_assignment set_null_if_empty(std::string_view value) const
        requires(is_writable && column_type::options.nullable_)
    {
        auto expr = db_expr::null_if(db_expr::value(value), db_expr::value(std::string_view{}));
        if constexpr (!column_type::options.enum_name_.view().empty()) {
            expr = expr.cast({.custom_name_ = column_type::options.enum_name_.view()});
        } else if constexpr (column_type::data_type != db_data_type::inferred &&
                             column_type::data_type != db_data_type::text &&
                             column_type::data_type != db_data_type::varchar &&
                             column_type::data_type != db_data_type::char_value) {
            expr = expr.cast({.data_type_ = column_type::data_type,
                .length_ = column_type::options.length_,
                .precision_ = column_type::options.precision_,
                .scale_ = column_type::options.scale_});
        }
        return db_assignment{field_name.view(), std::move(expr)};
    }
    [[nodiscard]] db_assignment set_default() const
        requires is_writable
    {
        return db_assignment{field_name.view(), db_expr::default_value()};
    }
    [[nodiscard]] db_assignment set_now() const
        requires is_writable
    {
        return db_assignment{field_name.view(), db_expr::now()};
    }
    [[nodiscard]] db_assignment set_excluded() const
        requires is_writable
    {
        return db_assignment{field_name.view(), excluded_expr()};
    }
    template <typename number_type = std::int64_t>
        requires(is_writable && is_numeric && (std::integral<number_type> || std::floating_point<number_type>) && !std::same_as<number_type, bool>)
    [[nodiscard]] db_assignment increment(number_type value = number_type{1}) const {
        return db_assignment{field_name.view(), to_expr() + value};
    }
    template <typename number_type>
        requires(is_writable && is_numeric && (std::integral<number_type> || std::floating_point<number_type>) && !std::same_as<number_type, bool>)
    [[nodiscard]] db_assignment operator+=(number_type value) const {
        return increment(value);
    }
    template <typename number_type = std::int64_t>
        requires(is_writable && is_numeric && (std::integral<number_type> || std::floating_point<number_type>) && !std::same_as<number_type, bool>)
    [[nodiscard]] db_assignment decrement(number_type value = number_type{1}) const {
        return db_assignment{field_name.view(), to_expr() - value};
    }
    template <typename number_type>
        requires(is_writable && is_numeric && (std::integral<number_type> || std::floating_point<number_type>) && !std::same_as<number_type, bool>)
    [[nodiscard]] db_assignment operator-=(number_type value) const {
        return decrement(value);
    }

private:
    template <typename t_type>
    [[nodiscard]] db_predicate compare_list(db_binary_operator op, std::span<const t_type> values) const {
        return db_predicate(detail::db_owned_expression::make_in_list(to_expr().owned(), op, values));
    }
    [[nodiscard]] db_predicate compare_subquery(db_binary_operator op, const db_query& subquery) const {
        return db_predicate(detail::db_owned_expression::make_in_subquery(to_expr().owned(), op, subquery));
    }
    [[nodiscard]] db_predicate compare_json(db_binary_operator op, std::string_view json_text) const {
        return to_expr().compare_expr(op, db_expr::value(json_text).cast(db_data_type::jsonb));
    }
    [[nodiscard]] db_predicate compare_json_keys(db_binary_operator op, std::span<const std::string_view> keys) const {
        return db_predicate(detail::db_owned_expression::make_json_keys_op(to_expr().owned(), op, keys));
    }
    template <typename t_type>
    db_predicate compare_array(db_binary_operator op, std::span<const t_type> values) const {
        static_assert(detail::is_pmr_vector<typename column_type::value_type>::value, "array predicates require an array column");
        using item_type = typename detail::is_pmr_vector<typename column_type::value_type>::value_type;
        constexpr auto type = column_type::options.data_type_ == db_data_type::inferred || column_type::options.data_type_ == db_data_type::array
                                  ? detail::db_entity_type_traits<item_type>::data_type
                                  : column_type::options.data_type_;
        const db_type_definition array_type{
            .data_type_ = type,
            .length_ = column_type::options.length_,
            .precision_ = column_type::options.precision_,
            .scale_ = column_type::options.scale_,
            .array_ = true,
        };
        return db_predicate(detail::db_owned_expression::make_array_op(to_expr().owned(), op, values, array_type));
    }

    std::string_view alias_{};
};

#define RUVIA_DB_COLUMN(name, value_type, ...) ::ruvia::db_column<::ruvia::fixed_string{#name}, value_type __VA_OPT__(, ) __VA_ARGS__>
#define RUVIA_DB_JOIN_COLUMN(local, referenced) ::ruvia::db_join_column<::ruvia::fixed_string{#local}, ::ruvia::fixed_string{#referenced}>
#define RUVIA_DB_JOIN_COLUMNS(...) ::ruvia::db_join_columns<__VA_ARGS__>
#define RUVIA_DB_INVERSE(name) ::ruvia::db_inverse<::ruvia::fixed_string{#name}>
#define RUVIA_DB_JOIN_TABLE(table, owner_columns_type, inverse_columns_type) ::ruvia::db_join_table<::ruvia::fixed_string{table}, owner_columns_type, inverse_columns_type>
#define RUVIA_DB_MANY_TO_ONE(name, target, ...) ::ruvia::db_many_to_one<::ruvia::fixed_string{#name}, target, __VA_ARGS__>
#define RUVIA_DB_ONE_TO_ONE(name, target, ...) ::ruvia::db_one_to_one<::ruvia::fixed_string{#name}, target, __VA_ARGS__>
#define RUVIA_DB_ONE_TO_MANY(name, target, inverse) ::ruvia::db_one_to_many<::ruvia::fixed_string{#name}, target, ::ruvia::fixed_string{#inverse}>
#define RUVIA_DB_MANY_TO_MANY(name, target, mapping_type) ::ruvia::db_many_to_many<::ruvia::fixed_string{#name}, target, mapping_type>
#define RUVIA_DB_ENTITY(name, table, ...)                                                                             \
    struct name final {                                                                                               \
    private:                                                                                                          \
        using storage_type = ::ruvia::detail::db_entity_storage<::ruvia::fixed_string{table}, __VA_ARGS__>;           \
        storage_type entity_;                                                                                         \
        template <typename>                                                                                           \
        friend struct ::ruvia::detail::db_entity_access;                                                              \
        storage_type& sql_storage() noexcept {                                                                        \
            return entity_;                                                                                           \
        }                                                                                                             \
                                                                                                                      \
    public:                                                                                                           \
        using sql_entity_type = name;                                                                                 \
        using columns_type = typename storage_type::columns_type;                                                     \
        using relations_type = typename storage_type::relations_type;                                                 \
        explicit name(std::pmr::memory_resource* resource = nullptr)                                                  \
            : entity_(resource) {}                                                                                    \
        template <typename owner_type>                                                                                \
            requires requires(owner_type& owner) {                                                                    \
                { owner.resource() } -> std::convertible_to<std::pmr::memory_resource*>;                              \
            }                                                                                                         \
        explicit name(owner_type& owner)                                                                              \
            : entity_(owner.resource()) {}                                                                            \
        static constexpr std::string_view table_name() noexcept {                                                     \
            return storage_type::table_name();                                                                        \
        }                                                                                                             \
        template <::ruvia::fixed_string field_name>                                                                   \
        static consteval std::size_t column_index() {                                                                 \
            return storage_type::template column_index<field_name>();                                                 \
        }                                                                                                             \
        template <::ruvia::fixed_string field_name>                                                                   \
        static consteval std::string_view column_name() noexcept {                                                    \
            return storage_type::template column_name<field_name>();                                                  \
        }                                                                                                             \
        template <::ruvia::fixed_string field_name>                                                                   \
        static constexpr ::ruvia::db_field_reference<name, field_name> column(std::string_view alias = {}) noexcept { \
            return ::ruvia::db_field_reference<name, field_name>(alias);                                              \
        }                                                                                                             \
        RUVIA_DETAIL_ENTITY_VALUE_API(entity_)                                                                        \
    };
namespace detail {
template <typename... columns>
struct db_projection_storage final {
    static_assert((::ruvia::is_db_column<columns>::value && ...));
    using type = ::ruvia::detail::entity_value_storage<columns...>;
};
}  // namespace detail

#define RUVIA_DB_PROJECTION(name, ...)                                                           \
    struct name final {                                                                          \
    private:                                                                                     \
        using storage_type = typename ::ruvia::detail::db_projection_storage<__VA_ARGS__>::type; \
        storage_type values_;                                                                    \
                                                                                                 \
    public:                                                                                      \
        using db_projection_type = name;                                                         \
        using columns_type = typename storage_type::columns_type;                                \
        explicit name(std::pmr::memory_resource* resource = nullptr)                             \
            : values_(resource) {}                                                               \
        template <typename owner_type>                                                           \
            requires requires(owner_type& owner) {                                               \
                { owner.resource() } -> std::convertible_to<std::pmr::memory_resource*>;         \
            }                                                                                    \
        explicit name(owner_type& owner)                                                         \
            : values_(owner.resource()) {}                                                       \
        RUVIA_DETAIL_ENTITY_VALUE_API(values_)                                                   \
    };

}  // namespace ruvia
