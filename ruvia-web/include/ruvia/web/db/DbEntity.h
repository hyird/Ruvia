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

#include "ruvia/web/EntityRows.h"
#include "ruvia/web/ModelTypes.h"
#include "ruvia/web/detail/entity/ValueStorage.h"

namespace ruvia {

namespace detail {
template <typename E>
struct DbEntityAccess;
struct DbResultAccess;
}  // namespace detail

enum class DbRelationKind : unsigned char { kOneToOne,
    kManyToOne,
    kOneToMany,
    kManyToMany };

template <FixedString Local, FixedString Referenced>
struct DbJoinColumn final {
    static constexpr auto local = Local;
    static constexpr auto referenced = Referenced;
};

template <typename... J>
struct DbJoinColumns final {
    using Tuple = std::tuple<J...>;
};
template <FixedString Name>
struct DbInverse final {
    static constexpr auto name = Name;
};
namespace detail {
template <typename T>
struct is_db_inverse : std::false_type {};
template <FixedString N>
struct is_db_inverse<DbInverse<N>> : std::true_type {};
template <typename T>
struct db_inverse_name {
    static constexpr auto value = FixedString{""};
};
template <FixedString N>
struct db_inverse_name<DbInverse<N>> {
    static constexpr auto value = N;
};
template <typename... T>
struct relation_join_tuple {
    using type = std::tuple<T...>;
};
template <typename T>
    requires requires { typename T::Tuple; }
struct relation_join_tuple<T> {
    using type = typename T::Tuple;
};
template <typename... T>
struct first_inverse_name {
    static constexpr auto value = FixedString{""};
};
template <typename T, typename... Rest>
struct first_inverse_name<T, Rest...> {
    static constexpr auto value = [] {
        if constexpr (is_db_inverse<T>::value) {
            return db_inverse_name<T>::value;
        } else {
            return first_inverse_name<Rest...>::value;
        }
    }();
};
}  // namespace detail
template <FixedString Table, typename Owner, typename Inverse>
struct DbJoinTable final {
    static constexpr auto name = Table;
    using OwnerColumns = typename Owner::Tuple;
    using InverseColumns = typename Inverse::Tuple;
};
namespace detail {
template <typename T>
struct is_db_join_table : std::false_type {};
template <FixedString N, typename O, typename I>
struct is_db_join_table<DbJoinTable<N, O, I>> : std::true_type {};
}  // namespace detail

template <FixedString Name, typename Target, typename... Js>
struct DbManyToOne final {
    static constexpr auto name = Name;
    using TargetEntity = Target;
    using JoinColumns = std::tuple<Js...>;
    static constexpr auto kind = DbRelationKind::kManyToOne;
    static constexpr bool isCollection = false;
    static constexpr bool isOwning = true;
    static constexpr auto inverseName = FixedString{""};
};
template <FixedString Name, typename Target, typename... Mapping>
struct DbOneToOne final {
    static constexpr auto name = Name;
    using TargetEntity = Target;
    static constexpr auto kind = DbRelationKind::kOneToOne;
    static constexpr bool isCollection = false;
    static constexpr bool isOwning = !(sizeof...(Mapping) == 1 && (detail::is_db_inverse<Mapping>::value && ...));
    using JoinColumns = typename detail::relation_join_tuple<Mapping...>::type;
    static constexpr auto inverseName = detail::first_inverse_name<Mapping...>::value;
};
template <FixedString Name, typename Target, FixedString Inverse>
struct DbOneToMany final {
    static constexpr auto name = Name;
    using TargetEntity = Target;
    static constexpr auto kind = DbRelationKind::kOneToMany;
    static constexpr bool isCollection = true;
    static constexpr bool isOwning = false;
    static constexpr auto inverseName = Inverse;
    using JoinColumns = std::tuple<>;
};
template <FixedString Name, typename Target, typename Mapping>
struct DbManyToMany final {
    static constexpr auto name = Name;
    using TargetEntity = Target;
    using JoinTable = Mapping;
    static constexpr auto kind = DbRelationKind::kManyToMany;
    static constexpr bool isCollection = true;
    static constexpr bool isOwning = detail::is_db_join_table<Mapping>::value;
    static constexpr auto inverseName = [] {
        if constexpr (detail::is_db_inverse<Mapping>::value) {
            return Mapping::name;
        } else {
            return FixedString{""};
        }
    }();
    using JoinColumns = std::tuple<>;
};

template <typename E, FixedString Name>
class DbFieldReference;

enum class DbDataType : unsigned char { kInferred,
    kBoolean,
    kSmallInt,
    kInteger,
    kBigInt,
    kReal,
    kDouble,
    kText,
    kChar,
    kVarchar,
    kNumeric,
    kJson,
    kJsonb,
    kUuid,
    kDate,
    kTimestamp,
    kTimestampTz,
    kInterval,
    kInet,
    kCidr,
    kBytea,
    kArray };
enum class DbGeneratedType : unsigned char { kNone,
    kStored,
    kVirtual };

template <typename EnumName = FixedString<1>, typename DefaultExpression = FixedString<1>>
struct DbColumnOptions final {
    DbDataType dataType{DbDataType::kInferred};
    bool primaryKey{false};
    bool generated{false};
    DbGeneratedType generatedType{DbGeneratedType::kNone};
    bool nullable{false};
    std::size_t length{0};
    unsigned precision{0};
    unsigned scale{0};
    EnumName enumName{""};
    DefaultExpression defaultExpression{""};
    constexpr bool operator==(const DbColumnOptions&) const = default;
};

namespace detail {

template <typename T>
struct DbEntityTypeTraits {
    using value_type = T;
    static constexpr DbDataType dataType = DbDataType::kInferred;
};
template <>
struct DbEntityTypeTraits<bool> {
    using value_type = bool;
    static constexpr DbDataType dataType = DbDataType::kBoolean;
};
template <>
struct DbEntityTypeTraits<std::int16_t> {
    using value_type = std::int16_t;
    static constexpr DbDataType dataType = DbDataType::kSmallInt;
};
template <>
struct DbEntityTypeTraits<std::int32_t> {
    using value_type = std::int32_t;
    static constexpr DbDataType dataType = DbDataType::kInteger;
};
template <>
struct DbEntityTypeTraits<std::int64_t> {
    using value_type = std::int64_t;
    static constexpr DbDataType dataType = DbDataType::kBigInt;
};
template <>
struct DbEntityTypeTraits<float> {
    using value_type = float;
    static constexpr DbDataType dataType = DbDataType::kReal;
};
template <>
struct DbEntityTypeTraits<double> {
    using value_type = double;
    static constexpr DbDataType dataType = DbDataType::kDouble;
};
template <>
struct DbEntityTypeTraits<std::pmr::string> {
    using value_type = std::pmr::string;
    static constexpr DbDataType dataType = DbDataType::kText;
};
template <>
struct DbEntityTypeTraits<String> {
    using value_type = String;
    static constexpr DbDataType dataType = DbDataType::kText;
};
template <typename T>
struct DbEntityTypeTraits<std::optional<T>> : DbEntityTypeTraits<T> {};
template <typename T>
struct DbEntityTypeTraits<std::pmr::vector<T>> {
    using value_type = std::pmr::vector<T>;
    static constexpr DbDataType dataType = DbDataType::kArray;
};

template <typename T>
struct DbRelationDeleter final {
    std::pmr::memory_resource* resource{nullptr};
    void operator()(T* value) const noexcept {
        if (value != nullptr) {
            std::pmr::polymorphic_allocator<T> allocator(resource);
            allocator.delete_object(value);
        }
    }
};

template <typename T, bool Collection = false>
struct DbRelationSlot final {
    enum class state_type : unsigned char { unset,
        null,
        value };
    using Stored = std::conditional_t<Collection, entity_rows<T>, T>;
    using pointer = std::unique_ptr<Stored, DbRelationDeleter<Stored>>;
    explicit DbRelationSlot(std::pmr::memory_resource* resource)
        : resource_(resource),
          value(nullptr, DbRelationDeleter<Stored>{resource}) {}
    DbRelationSlot(const DbRelationSlot&) = delete;
    DbRelationSlot& operator=(const DbRelationSlot&) = delete;
    DbRelationSlot(DbRelationSlot&& other) noexcept
        : resource_(other.resource_),
          state(std::exchange(other.state, state_type::unset)),
          value(std::move(other.value)) {}
    DbRelationSlot& operator=(DbRelationSlot&&) = delete;
    std::pmr::memory_resource* resource_;
    state_type state{state_type::unset};
    pointer value;
    void reset() noexcept {
        value.reset();
        state = state_type::unset;
    }
};

template <template <typename> class Predicate, typename... Ts>
struct tuple_filter;
template <template <typename> class Predicate>
struct tuple_filter<Predicate> {
    using type = std::tuple<>;
};
template <template <typename> class Predicate, typename T, typename... Ts>
struct tuple_filter<Predicate, T, Ts...> {
    using tail = typename tuple_filter<Predicate, Ts...>::type;
    using type = std::conditional_t<Predicate<T>::value, decltype(std::tuple_cat(std::declval<std::tuple<T>>(), std::declval<tail>())), tail>;
};
template <template <typename> class Predicate, typename... Ts>
using tuple_filter_t = typename tuple_filter<Predicate, Ts...>::type;

}  // namespace detail

template <FixedString Name, typename T, DbColumnOptions Options = DbColumnOptions{}>
struct DbColumn final {
    static_assert(!detail::is_optional<T>::value, "use nullable column options and setNull for entity fields; optional is only for array elements");
    static constexpr auto name = Name;
    using value_type = T;
    static constexpr auto options = Options;
    static_assert(!std::is_same_v<T, char> && !std::is_same_v<T, std::string_view>,
        "database entity columns must own their values");
    static constexpr DbDataType dataType = Options.dataType == DbDataType::kInferred
                                               ? detail::DbEntityTypeTraits<T>::dataType
                                               : Options.dataType;
};

template <typename T>
struct is_db_column : std::false_type {};
template <FixedString Name, typename T, DbColumnOptions Options>
struct is_db_column<DbColumn<Name, T, Options>> : std::true_type {};
template <typename T>
struct is_db_relation : std::false_type {};
template <FixedString Name, typename Target, typename... Joins>
struct is_db_relation<DbManyToOne<Name, Target, Joins...>> : std::true_type {};
template <FixedString Name, typename Target, typename... Mapping>
struct is_db_relation<DbOneToOne<Name, Target, Mapping...>> : std::true_type {};
template <FixedString Name, typename Target, FixedString Inverse>
struct is_db_relation<DbOneToMany<Name, Target, Inverse>> : std::true_type {};
template <FixedString Name, typename Target, typename Mapping>
struct is_db_relation<DbManyToMany<Name, Target, Mapping>> : std::true_type {};

template <FixedString Table, typename... Members>
class DbEntity final {
    static_assert(((is_db_column<Members>::value || is_db_relation<Members>::value) && ...),
        "SQL entities require RUVIA_DB_COLUMN or SQL relation descriptors");
    using ColumnsTuple = detail::tuple_filter_t<is_db_column, Members...>;
    using RelationsTuple = detail::tuple_filter_t<is_db_relation, Members...>;
    static_assert(detail::uniqueEntityColumns<Members...>(), "duplicate database entity member name");
    template <typename R>
    using RelationSlot = detail::DbRelationSlot<typename R::TargetEntity, R::isCollection>;
    template <std::size_t... I>
    static auto makeRelationSlots(std::pmr::memory_resource* r, std::index_sequence<I...>) {
        return std::tuple<RelationSlot<std::tuple_element_t<I, RelationsTuple>>...>(RelationSlot<std::tuple_element_t<I, RelationsTuple>>(r)...);
    }
    using column_storage = typename detail::entity_storage_from_tuple<ColumnsTuple>::type;
    using RelationSlots = decltype(makeRelationSlots(nullptr, std::make_index_sequence<std::tuple_size_v<RelationsTuple>>{}));
    template <FixedString Name>
    static consteval std::size_t index() {
        return column_storage::template index<Name>();
    }
    template <FixedString Name>
    static consteval std::size_t relationIndex() {
        constexpr auto result = []<std::size_t... I>(std::index_sequence<I...>) consteval {
            constexpr bool matches[] = {std::tuple_element_t<I, RelationsTuple>::name == Name...};
            for (std::size_t i = 0; i < sizeof...(I); ++i) {
                if (matches[i]) {
                    return i;
                }
            }
            return sizeof...(I);
        }(std::make_index_sequence<std::tuple_size_v<RelationsTuple>>{});
        static_assert(result < std::tuple_size_v<RelationsTuple>, "unknown database entity relation");
        return result;
    }
    template <FixedString Name>
    static consteval bool isRelation() {
        constexpr bool matches[] = {Members::name == Name...};
        constexpr bool columns[] = {is_db_column<Members>::value...};
        for (std::size_t i = 0; i < sizeof...(Members); ++i) {
            if (matches[i]) {
                return !columns[i];
            }
        }
        return false;
    }
    template <FixedString Name>
    auto& relationSlot() {
        return std::get<relationIndex<Name>()>(relationSlots_);
    }
    template <FixedString Name>
    const auto& relationSlot() const {
        return std::get<relationIndex<Name>()>(relationSlots_);
    }

public:
    using SqlEntityType = DbEntity;
    using Columns = ColumnsTuple;
    using Relations = RelationsTuple;
    static constexpr std::string_view tableName() noexcept {
        return Table.view();
    }
    template <FixedString Name>
    static consteval std::size_t columnIndex() {
        return index<Name>();
    }
    template <FixedString Name>
    static consteval std::string_view columnName() {
        (void)index<Name>();
        return Name.view();
    }
    template <FixedString Name>
    static DbFieldReference<DbEntity, Name> column();
    explicit DbEntity(std::pmr::memory_resource* resource = nullptr)
        : columns_(resource),
          relationSlots_(makeRelationSlots(columns_.resource(), std::make_index_sequence<std::tuple_size_v<RelationsTuple>>{})) {}
    DbEntity(const DbEntity&) = delete;
    DbEntity& operator=(const DbEntity&) = delete;
    DbEntity(DbEntity&&) noexcept = default;
    template <FixedString Name>
    auto& get() & {
        if constexpr (isRelation<Name>()) {
            auto& s = relationSlot<Name>();
            if (s.state != decltype(s.state)::value) {
                throw std::logic_error("database entity relation is not loaded");
            }
            return *s.value;
        } else {
            return columns_.template get<Name>();
        }
    }
    template <FixedString Name>
    const auto& get() const& {
        return const_cast<DbEntity*>(this)->get<Name>();
    }
    template <FixedString Name>
    const auto& get() const&& = delete;
    template <FixedString Name, typename V>
    void set(V&& value) {
        static_assert(!isRelation<Name>(), "relations cannot be assigned; use relation loading access");
        columns_.template set<Name>(std::forward<V>(value));
    }
    template <FixedString Name>
    void setNull()
        requires(!isRelation<Name>() && std::tuple_element_t<index<Name>(), Columns>::options.nullable)
    {
        columns_.template set_null<Name>();
    }
    template <FixedString Name>
    void reset() {
        if constexpr (isRelation<Name>()) {
            relationSlot<Name>().reset();
        } else {
            columns_.template reset<Name>();
        }
    }
    template <FixedString Name>
    bool isSet() const {
        if constexpr (isRelation<Name>()) {
            return relationSlot<Name>().state != decltype(relationSlot<Name>().state)::unset;
        } else {
            return columns_.template is_set<Name>();
        }
    }
    template <FixedString Name>
    bool isNull() const {
        if constexpr (isRelation<Name>()) {
            return relationSlot<Name>().state == decltype(relationSlot<Name>().state)::null;
        } else {
            return columns_.template is_null<Name>();
        }
    }
    std::pmr::memory_resource* resource() const noexcept {
        return columns_.resource();
    }

private:
    template <typename>
    friend struct detail::DbEntityAccess;
    DbEntity& sql_storage() noexcept {
        return *this;
    }
    column_storage columns_;
    RelationSlots relationSlots_;
};

namespace detail {
template <typename columns>
inline constexpr bool sql_columns = []<std::size_t... indices>(std::index_sequence<indices...>) {
    return (is_db_column<std::tuple_element_t<indices, columns>>::value && ...);
}(std::make_index_sequence<std::tuple_size_v<columns>>{});
}  // namespace detail

template <typename entity>
concept sql_entity = requires(const entity& value) {
    typename entity::Columns;
    typename entity::Relations;
    typename entity::SqlEntityType;
    requires std::same_as<entity, typename entity::SqlEntityType>;
    requires detail::sql_columns<typename entity::Columns>;
    { entity::tableName() } -> std::convertible_to<std::string_view>;
    { value.resource() } -> std::same_as<std::pmr::memory_resource*>;
};

template <typename projection>
concept sql_projection = requires(const projection& value) {
    typename projection::Columns;
    typename projection::DbProjectionType;
    requires std::same_as<projection, typename projection::DbProjectionType>;
    requires detail::sql_columns<typename projection::Columns>;
    { value.resource() } -> std::same_as<std::pmr::memory_resource*>;
};

#define RUVIA_DB_COLUMN(Name, Type, ...) ::ruvia::DbColumn<::ruvia::FixedString{#Name}, Type __VA_OPT__(, ) __VA_ARGS__>
#define RUVIA_DB_JOIN_COLUMN(Local, Referenced) ::ruvia::DbJoinColumn<::ruvia::FixedString{#Local}, ::ruvia::FixedString{#Referenced}>
#define RUVIA_DB_JOIN_COLUMNS(...) ::ruvia::DbJoinColumns<__VA_ARGS__>
#define RUVIA_DB_INVERSE(Name) ::ruvia::DbInverse<::ruvia::FixedString{#Name}>
#define RUVIA_DB_JOIN_TABLE(Table, OwnerColumns, InverseColumns) ::ruvia::DbJoinTable<::ruvia::FixedString{Table}, OwnerColumns, InverseColumns>
#define RUVIA_DB_MANY_TO_ONE(Name, Target, ...) ::ruvia::DbManyToOne<::ruvia::FixedString{#Name}, Target, __VA_ARGS__>
#define RUVIA_DB_ONE_TO_ONE(Name, Target, ...) ::ruvia::DbOneToOne<::ruvia::FixedString{#Name}, Target, __VA_ARGS__>
#define RUVIA_DB_ONE_TO_MANY(Name, Target, Inverse) ::ruvia::DbOneToMany<::ruvia::FixedString{#Name}, Target, ::ruvia::FixedString{#Inverse}>
#define RUVIA_DB_MANY_TO_MANY(Name, Target, Mapping) ::ruvia::DbManyToMany<::ruvia::FixedString{#Name}, Target, Mapping>
#define RUVIA_DB_ENTITY(Name, Table, ...)                                                 \
    struct Name final {                                                                   \
    private:                                                                              \
        using storage_type = ::ruvia::DbEntity<::ruvia::FixedString{Table}, __VA_ARGS__>; \
        storage_type entity_;                                                             \
        template <typename>                                                               \
        friend struct ::ruvia::detail::DbEntityAccess;                                    \
        storage_type& sql_storage() noexcept {                                            \
            return entity_;                                                               \
        }                                                                                 \
                                                                                          \
    public:                                                                               \
        using SqlEntityType = Name;                                                       \
        using Columns = typename storage_type::Columns;                                   \
        using Relations = typename storage_type::Relations;                               \
        explicit Name(std::pmr::memory_resource* resource = nullptr)                      \
            : entity_(resource) {}                                                        \
        static constexpr std::string_view tableName() noexcept {                          \
            return storage_type::tableName();                                             \
        }                                                                                 \
        template <::ruvia::FixedString name>                                              \
        static consteval std::size_t columnIndex() {                                      \
            return storage_type::template columnIndex<name>();                            \
        }                                                                                 \
        template <::ruvia::FixedString name>                                              \
        static consteval std::string_view columnName() {                                  \
            return storage_type::template columnName<name>();                             \
        }                                                                                 \
        template <::ruvia::FixedString name>                                              \
        static ::ruvia::DbFieldReference<Name, name> column() {                           \
            return {};                                                                    \
        }                                                                                 \
        RUVIA_DETAIL_ENTITY_VALUE_API(entity_, setNull, isSet, isNull)                    \
    };

}  // namespace ruvia
