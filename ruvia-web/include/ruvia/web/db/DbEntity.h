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
#include <vector>

#include "ruvia/web/ModelTypes.h"

namespace ruvia {

namespace detail {
template <typename E>
struct DbEntityAccess;
}
template <typename Entity>
class DbEntityRows;

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

struct DbColumnOptions final {
    DbDataType dataType{DbDataType::kInferred};
    bool primaryKey{false};
    bool generated{false};
    DbGeneratedType generatedType{DbGeneratedType::kNone};
    bool nullable{false};
    std::size_t length{0};
    unsigned precision{0};
    unsigned scale{0};
    constexpr bool operator==(const DbColumnOptions&) const = default;
};

namespace detail {

template <typename T>
struct DbEntityTypeTraits {
    using value_type = T;
    static constexpr DbDataType dataType = DbDataType::kInferred;
};
template <typename T>
struct IsPmrVector : std::false_type {};
template <typename T>
struct IsPmrVector<std::pmr::vector<T>> : std::true_type {
    using value_type = T;
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
struct DbEntitySlot {
    enum class State : unsigned char { kUnset,
        kNull,
        kValue };
    explicit DbEntitySlot(std::pmr::memory_resource* resource)
        : resource_(resource),
          value(makeValue(resource)) {}
    static T makeValue(std::pmr::memory_resource* resource) {
        if constexpr (std::is_same_v<T, String>) {
            return String(ModelOptions{.resource = resource});
        } else if constexpr (std::is_same_v<T, std::pmr::string> || IsPmrVector<T>::value) {
            return T(resource);
        } else {
            return T{};
        }
    }
    void clear() {
        // Even an empty container can allocate (for example, a debug iterator
        // proxy). Keep the current value alive until that construction succeeds.
        auto empty = makeValue(resource_);
        static_assert(std::is_nothrow_move_constructible_v<T>);
        value.~T();
        std::construct_at(&value, std::move(empty));
    }
    std::pmr::memory_resource* resource_{nullptr};
    State state{State::kUnset};
    T value;
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
    enum class State : unsigned char { kUnset,
        kNull,
        kValue };
    using Stored = std::conditional_t<Collection, DbEntityRows<T>, T>;
    using pointer = std::unique_ptr<Stored, DbRelationDeleter<Stored>>;
    explicit DbRelationSlot(std::pmr::memory_resource* resource)
        : resource_(resource),
          value(nullptr, DbRelationDeleter<Stored>{resource}) {}
    DbRelationSlot(const DbRelationSlot&) = delete;
    DbRelationSlot& operator=(const DbRelationSlot&) = delete;
    DbRelationSlot(DbRelationSlot&& other) noexcept
        : resource_(other.resource_),
          state(std::exchange(other.state, State::kUnset)),
          value(std::move(other.value)) {}
    DbRelationSlot& operator=(DbRelationSlot&&) = delete;
    std::pmr::memory_resource* resource_;
    State state{State::kUnset};
    pointer value;
    void reset() noexcept {
        value.reset();
        state = State::kUnset;
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

template <FixedString Name, typename... ColumnTypes>
consteval std::size_t entityColumnIndex() {
    constexpr bool matches[] = {ColumnTypes::name == Name...};
    for (std::size_t i = 0; i < sizeof...(ColumnTypes); ++i) {
        if (matches[i]) {
            return i;
        }
    }
    return sizeof...(ColumnTypes);
}
template <typename... ColumnTypes>
consteval bool uniqueEntityColumns() {
    constexpr std::string_view names[] = {ColumnTypes::name.view()...};
    for (std::size_t i = 0; i < sizeof...(ColumnTypes); ++i) {
        for (std::size_t j = i + 1; j < sizeof...(ColumnTypes); ++j) {
            if (names[i] == names[j]) {
                return false;
            }
        }
    }
    return true;
}

template <typename T>
struct is_optional : std::false_type {};
template <typename T>
struct is_optional<std::optional<T>> : std::true_type {
    using value_type = T;
};

template <typename T, typename V>
void assignEntityValue(T& out, V&& value, std::pmr::memory_resource* resource) {
    if constexpr (std::is_same_v<T, String>) {
        out.assignOwned(std::string_view(value));
    } else if constexpr (std::is_same_v<T, std::pmr::string>) {
        out = std::forward<V>(value);
    } else if constexpr (is_optional<T>::value) {
        if (!value) {
            out.reset();
        } else {
            using Element = typename is_optional<T>::value_type;
            auto owned = DbEntitySlot<Element>::makeValue(resource);
            assignEntityValue(owned, *value, resource);
            out.emplace(std::move(owned));
        }
    } else if constexpr (IsPmrVector<T>::value) {
        using Element = typename IsPmrVector<T>::value_type;
        if constexpr (std::is_arithmetic_v<Element> || std::is_same_v<Element, std::pmr::string>) {
            out = std::forward<V>(value);
        } else {
            T owned(resource);
            owned.reserve(value.size());
            for (const auto& item : value) {
                auto element = DbEntitySlot<Element>::makeValue(resource);
                assignEntityValue(element, item, resource);
                owned.push_back(std::move(element));
            }
            out = std::move(owned);
        }
    } else {
        out = std::forward<V>(value);
    }
}

}  // namespace detail

template <FixedString Name, typename T, DbColumnOptions Options = {}>
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
class DbEntity {
    using ColumnsTuple = detail::tuple_filter_t<is_db_column, Members...>;
    using RelationsTuple = detail::tuple_filter_t<is_db_relation, Members...>;
    static_assert(detail::uniqueEntityColumns<Members...>(), "duplicate database entity member name");
    template <typename C>
    using ColumnSlot = detail::DbEntitySlot<typename C::value_type>;
    template <typename R>
    using RelationSlot = detail::DbRelationSlot<typename R::TargetEntity, R::isCollection>;
    template <std::size_t... I>
    static auto makeColumnSlots(std::pmr::memory_resource* r, std::index_sequence<I...>) {
        return std::tuple<ColumnSlot<std::tuple_element_t<I, ColumnsTuple>>...>(ColumnSlot<std::tuple_element_t<I, ColumnsTuple>>(r)...);
    }
    template <std::size_t... I>
    static auto makeRelationSlots(std::pmr::memory_resource* r, std::index_sequence<I...>) {
        return std::tuple<RelationSlot<std::tuple_element_t<I, RelationsTuple>>...>(RelationSlot<std::tuple_element_t<I, RelationsTuple>>(r)...);
    }
    using ColumnSlots = decltype(makeColumnSlots(nullptr, std::make_index_sequence<std::tuple_size_v<ColumnsTuple>>{}));
    using RelationSlots = decltype(makeRelationSlots(nullptr, std::make_index_sequence<std::tuple_size_v<RelationsTuple>>{}));
    template <FixedString Name>
    static consteval std::size_t index() {
        constexpr auto result = []<std::size_t... I>(std::index_sequence<I...>) consteval {
            constexpr bool matches[] = {std::tuple_element_t<I, ColumnsTuple>::name == Name...};
            for (std::size_t i = 0; i < sizeof...(I); ++i) {
                if (matches[i]) {
                    return i;
                }
            }
            return sizeof...(I);
        }(std::make_index_sequence<std::tuple_size_v<ColumnsTuple>>{});
        static_assert(result < std::tuple_size_v<ColumnsTuple>, "unknown database entity column");
        return result;
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
    auto& slot() {
        return std::get<index<Name>()>(columnSlots_);
    }
    template <FixedString Name>
    const auto& slot() const {
        return std::get<index<Name>()>(columnSlots_);
    }
    template <FixedString Name>
    auto& relationSlot() {
        return std::get<relationIndex<Name>()>(relationSlots_);
    }
    template <FixedString Name>
    const auto& relationSlot() const {
        return std::get<relationIndex<Name>()>(relationSlots_);
    }
    template <FixedString Name>
    static consteval std::size_t memberIndex() {
        constexpr bool matches[] = {Members::name == Name...};
        for (std::size_t i = 0; i < sizeof...(Members); ++i) {
            if (matches[i]) {
                return i;
            }
        }
        return sizeof...(Members);
    }

public:
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
        : resource_(detail::pmrResourceOrDefault(resource)),
          columnSlots_(makeColumnSlots(resource_, std::make_index_sequence<std::tuple_size_v<ColumnsTuple>>{})),
          relationSlots_(makeRelationSlots(resource_, std::make_index_sequence<std::tuple_size_v<RelationsTuple>>{})) {}
    DbEntity(const DbEntity&) = delete;
    DbEntity& operator=(const DbEntity&) = delete;
    DbEntity(DbEntity&&) noexcept = default;
    template <FixedString Name>
    auto& get() & {
        if constexpr (isRelation<Name>()) {
            auto& s = relationSlot<Name>();
            if (s.state != decltype(s.state)::kValue) {
                throw std::logic_error("database entity relation is not loaded");
            }
            return *s.value;
        } else {
            auto& s = slot<Name>();
            if (s.state != decltype(s.state)::kValue) {
                throw std::logic_error("database entity value is not set");
            }
            return s.value;
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
        auto& s = slot<Name>();
        detail::assignEntityValue(s.value, std::forward<V>(value), resource_);
        s.state = decltype(s.state)::kValue;
    }
    template <FixedString Name>
    void setNull()
        requires(!isRelation<Name>() && std::tuple_element_t<index<Name>(), Columns>::options.nullable)
    {
        slot<Name>().clear();
        slot<Name>().state = decltype(slot<Name>().state)::kNull;
    }
    template <FixedString Name>
    void reset() {
        if constexpr (isRelation<Name>()) {
            relationSlot<Name>().reset();
        } else {
            slot<Name>().clear();
            slot<Name>().state = decltype(slot<Name>().state)::kUnset;
        }
    }
    template <FixedString Name>
    bool isSet() const {
        if constexpr (isRelation<Name>()) {
            return relationSlot<Name>().state != decltype(relationSlot<Name>().state)::kUnset;
        } else {
            return slot<Name>().state != decltype(slot<Name>().state)::kUnset;
        }
    }
    template <FixedString Name>
    bool isNull() const {
        if constexpr (isRelation<Name>()) {
            return relationSlot<Name>().state == decltype(relationSlot<Name>().state)::kNull;
        } else {
            return slot<Name>().state == decltype(slot<Name>().state)::kNull;
        }
    }
    std::pmr::memory_resource* resource() const noexcept {
        return resource_;
    }

private:
    template <typename>
    friend struct detail::DbEntityAccess;
    std::pmr::memory_resource* resource_;
    ColumnSlots columnSlots_;
    RelationSlots relationSlots_;
};

template <typename Entity>
class DbEntityRows final {
public:
    explicit DbEntityRows(std::pmr::memory_resource* resource = nullptr)
        : rows_(detail::pmrResourceOrDefault(resource)) {}
    DbEntityRows(const DbEntityRows&) = delete;
    DbEntityRows& operator=(const DbEntityRows&) = delete;
    DbEntityRows(DbEntityRows&&) noexcept = default;
    DbEntityRows& operator=(DbEntityRows&&) = delete;
    std::size_t size() const noexcept {
        return rows_.size();
    }
    bool empty() const noexcept {
        return rows_.empty();
    }
    const Entity& operator[](std::size_t i) const& noexcept {
        return rows_[i];
    }
    Entity& operator[](std::size_t i) & noexcept {
        return rows_[i];
    }
    const Entity& operator[](std::size_t) const&& = delete;
    const Entity* begin() const& noexcept {
        return rows_.data();
    }
    const Entity* end() const& noexcept {
        return rows_.data() + rows_.size();
    }
    const Entity* begin() const&& = delete;
    const Entity* end() const&& = delete;
    void push_back(Entity&& entity) {
        if (entity.resource() != rows_.get_allocator().resource()) {
            throw std::invalid_argument("entity result elements must use the result's memory resource");
        }
        rows_.push_back(std::move(entity));
    }

private:
    std::pmr::vector<Entity> rows_;
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
#define RUVIA_DB_ENTITY(Name, Table, ...)                                             \
    struct Name final : ::ruvia::DbEntity<::ruvia::FixedString{Table}, __VA_ARGS__> { \
        using ::ruvia::DbEntity<::ruvia::FixedString{Table}, __VA_ARGS__>::DbEntity;  \
    };

}  // namespace ruvia
