#pragma once

#include <concepts>
#include <cstddef>
#include <memory>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/web/detail/entity/value_storage.h"
#include "ruvia/web/entity_rows.h"
#include "ruvia/web/model_types.h"

namespace ruvia {

namespace detail {
template <typename e_type>
struct db_entity_access;
struct db_result_access;
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

template <typename e_type, fixed_string name>
class db_field_reference;

enum class db_data_type : unsigned char { inferred,
    boolean,
    small_int,
    integer,
    big_int,
    real,
    double_value,
    text,
    char_value,
    varchar,
    numeric,
    json,
    jsonb,
    uuid,
    date,
    timestamp,
    timestamp_tz,
    interval,
    inet,
    cidr,
    bytea,
    array };
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
struct db_entity_type_traits<std::optional<t_type>> : db_entity_type_traits<t_type> {};
template <typename t_type>
struct db_entity_type_traits<std::pmr::vector<t_type>> {
    using value_type = std::pmr::vector<t_type>;
    static constexpr db_data_type data_type = db_data_type::array;
};

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

template <fixed_string field_name, typename t_type, db_column_options options_type = db_column_options{}>
struct db_column final {
    static_assert(!detail::is_optional<t_type>::value, "use nullable column options and set_null for entity fields; optional is only for array elements");
    static constexpr auto name = field_name;
    using value_type = t_type;
    static constexpr auto options = options_type;
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

template <fixed_string table, typename... members_type>
class db_entity final {
    static_assert(((is_db_column<members_type>::value || is_db_relation<members_type>::value) && ...),
        "SQL entities require RUVIA_DB_COLUMN or SQL relation descriptors");
    using columns_tuple_type = detail::tuple_filter_t<is_db_column, members_type...>;
    using relations_tuple_type = detail::tuple_filter_t<is_db_relation, members_type...>;
    static_assert(detail::unique_entity_columns<members_type...>(), "duplicate database entity member name");
    template <typename r_type>
    using relation_slot_type = detail::db_relation_slot<typename r_type::target_entity_type, r_type::is_collection>;
    template <std::size_t... indexes>
    static auto make_relation_slots(std::pmr::memory_resource* resource, std::index_sequence<indexes...>) {
        return std::tuple<relation_slot_type<std::tuple_element_t<indexes, relations_tuple_type>>...>(relation_slot_type<std::tuple_element_t<indexes, relations_tuple_type>>(resource)...);
    }
    using column_storage = typename detail::entity_storage_from_tuple<columns_tuple_type>::type;
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
    using sql_entity_type = db_entity;
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
    static consteval std::string_view column_name() {
        (void)index<name>();
        return name.view();
    }
    template <fixed_string name>
    static db_field_reference<db_entity, name> column();
    explicit db_entity(std::pmr::memory_resource* resource = nullptr)
        : columns_(resource),
          relation_slots_(make_relation_slots(columns_.resource(), std::make_index_sequence<std::tuple_size_v<relations_tuple_type>>{})) {}
    db_entity(const db_entity&) = delete;
    db_entity& operator=(const db_entity&) = delete;
    db_entity(db_entity&&) noexcept = default;
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
        return const_cast<db_entity*>(this)->get<name>();
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
    friend struct detail::db_entity_access;
    db_entity& sql_storage() noexcept {
        return *this;
    }
    column_storage columns_;
    relation_slots_type relation_slots_;
};

namespace detail {
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

#define RUVIA_DB_COLUMN(name, type_type, ...) ::ruvia::db_column<::ruvia::fixed_string{#name}, type_type __VA_OPT__(, ) __VA_ARGS__>
#define RUVIA_DB_JOIN_COLUMN(local, referenced) ::ruvia::db_join_column<::ruvia::fixed_string{#local}, ::ruvia::fixed_string{#referenced}>
#define RUVIA_DB_JOIN_COLUMNS(...) ::ruvia::db_join_columns<__VA_ARGS__>
#define RUVIA_DB_INVERSE(name) ::ruvia::db_inverse<::ruvia::fixed_string{#name}>
#define RUVIA_DB_JOIN_TABLE(table, owner_columns_type, inverse_columns_type) ::ruvia::db_join_table<::ruvia::fixed_string{table}, owner_columns_type, inverse_columns_type>
#define RUVIA_DB_MANY_TO_ONE(name, target, ...) ::ruvia::db_many_to_one<::ruvia::fixed_string{#name}, target, __VA_ARGS__>
#define RUVIA_DB_ONE_TO_ONE(name, target, ...) ::ruvia::db_one_to_one<::ruvia::fixed_string{#name}, target, __VA_ARGS__>
#define RUVIA_DB_ONE_TO_MANY(name, target, inverse) ::ruvia::db_one_to_many<::ruvia::fixed_string{#name}, target, ::ruvia::fixed_string{#inverse}>
#define RUVIA_DB_MANY_TO_MANY(name, target, mapping_type) ::ruvia::db_many_to_many<::ruvia::fixed_string{#name}, target, mapping_type>
#define RUVIA_DB_ENTITY(name, table, ...)                                                   \
    struct name final {                                                                     \
    private:                                                                                \
        using storage_type = ::ruvia::db_entity<::ruvia::fixed_string{table}, __VA_ARGS__>; \
        storage_type entity_;                                                               \
        template <typename>                                                                 \
        friend struct ::ruvia::detail::db_entity_access;                                    \
        storage_type& sql_storage() noexcept {                                              \
            return entity_;                                                                 \
        }                                                                                   \
                                                                                            \
    public:                                                                                 \
        using sql_entity_type = name;                                                       \
        using columns_type = typename storage_type::columns_type;                           \
        using relations_type = typename storage_type::relations_type;                       \
        explicit name(std::pmr::memory_resource* resource = nullptr)                        \
            : entity_(resource) {}                                                          \
        static constexpr std::string_view table_name() noexcept {                           \
            return storage_type::table_name();                                              \
        }                                                                                   \
        template <::ruvia::fixed_string field_name>                                         \
        static consteval std::size_t column_index() {                                       \
            return storage_type::template column_index<field_name>();                       \
        }                                                                                   \
        template <::ruvia::fixed_string field_name>                                         \
        static consteval std::string_view column_name() {                                   \
            return storage_type::template column_name<field_name>();                        \
        }                                                                                   \
        template <::ruvia::fixed_string field_name>                                         \
        static ::ruvia::db_field_reference<name, field_name> column() {                     \
            return {};                                                                      \
        }                                                                                   \
        RUVIA_DETAIL_ENTITY_VALUE_API(entity_, set_null, is_set, is_null)                   \
    };

}  // namespace ruvia
