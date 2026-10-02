#pragma once

#include <cstddef>
#include <memory_resource>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>

#include "ruvia/core/memory/PmrResource.h"
#include "ruvia/web/detail/entity/ValueStorage.h"
#include "ruvia/web/detail/redis/RedisEntityTraits.h"
#include "ruvia/web/redis/RedisPredicate.h"

namespace ruvia {

struct RedisColumnOptions final {
    bool primaryKey{false};
    bool nullable{false};
};

template <FixedString Name, typename T, RedisColumnOptions Options = {}>
struct RedisColumn final {
    static_assert(detail::isRedisEntityScalar<T>, "Redis columns require owning scalar values");
    static_assert(!Options.primaryKey || (detail::isRedisEntityPrimaryKeyScalar<T> && !Options.nullable),
        "Redis primary keys must be non-nullable strings or integers");
    static constexpr auto name = Name;
    using value_type = T;
    static constexpr auto options = Options;
};

namespace detail {
template <typename T>
struct IsRedisColumn : std::false_type {};
template <FixedString Name, typename T, RedisColumnOptions Options>
struct IsRedisColumn<RedisColumn<Name, T, Options>> : std::true_type {};
}  // namespace detail

template <FixedString Prefix, typename... ColumnTypes>
class RedisEntity {
    static_assert((detail::IsRedisColumn<ColumnTypes>::value && ...),
        "Redis entities require RUVIA_REDIS_COLUMN descriptors");
    static_assert((std::size_t{ColumnTypes::options.primaryKey} + ... + std::size_t{0}) == 1,
        "Redis entities require exactly one primary key");
    static_assert(detail::uniqueEntityColumns<ColumnTypes...>(), "duplicate Redis entity column name");
    using slots_type = std::tuple<detail::entity_value_slot<typename ColumnTypes::value_type>...>;
    template <FixedString Name>
    static consteval std::size_t index() {
        constexpr auto value = detail::entityColumnIndex<Name, ColumnTypes...>();
        static_assert(value < sizeof...(ColumnTypes), "unknown Redis entity column");
        return value;
    }

public:
    using RedisEntityType = RedisEntity;
    using Columns = std::tuple<ColumnTypes...>;
    static constexpr std::string_view prefix() noexcept {
        return Prefix.view();
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
    static redis_field_reference<RedisEntity, Name> field() {
        return {};
    }

    explicit RedisEntity(std::pmr::memory_resource* resource = nullptr)
        : resource_(detail::pmrResourceOrDefault(resource)),
          slots_(detail::entity_value_slot<typename ColumnTypes::value_type>(resource_)...) {}
    RedisEntity(const RedisEntity&) = delete;
    RedisEntity& operator=(const RedisEntity&) = delete;
    RedisEntity(RedisEntity&&) noexcept = default;
    RedisEntity& operator=(RedisEntity&&) = delete;

    template <FixedString Name>
    auto& get() & {
        auto& slot = std::get<index<Name>()>(slots_);
        if (slot.state != decltype(slot.state)::value) {
            throw std::logic_error("Redis entity value is not set");
        }
        return slot.value;
    }
    template <FixedString Name>
    const auto& get() const& {
        const auto& slot = std::get<index<Name>()>(slots_);
        if (slot.state != decltype(slot.state)::value) {
            throw std::logic_error("Redis entity value is not set");
        }
        return slot.value;
    }
    template <FixedString Name>
    const auto& get() const&& = delete;
    template <FixedString Name, typename Value>
    void set(Value&& value) {
        auto& slot = std::get<index<Name>()>(slots_);
        detail::assignEntityValue(slot.value, std::forward<Value>(value), resource_);
        slot.state = decltype(slot.state)::value;
    }
    template <FixedString Name>
    void setNull()
        requires(std::tuple_element_t<index<Name>(), Columns>::options.nullable)
    {
        auto& slot = std::get<index<Name>()>(slots_);
        slot.clear();
        slot.state = decltype(slot.state)::null;
    }
    template <FixedString Name>
    void reset() {
        auto& slot = std::get<index<Name>()>(slots_);
        slot.clear();
        slot.state = decltype(slot.state)::unset;
    }
    template <FixedString Name>
    bool isSet() const {
        const auto& slot = std::get<index<Name>()>(slots_);
        return slot.state != decltype(slot.state)::unset;
    }
    template <FixedString Name>
    bool isNull() const {
        const auto& slot = std::get<index<Name>()>(slots_);
        return slot.state == decltype(slot.state)::null;
    }
    std::pmr::memory_resource* resource() const noexcept {
        return resource_;
    }

private:
    std::pmr::memory_resource* resource_;
    slots_type slots_;
};

#define RUVIA_REDIS_COLUMN(Name, Type, ...) ::ruvia::RedisColumn<::ruvia::FixedString{#Name}, Type __VA_OPT__(, ) __VA_ARGS__>
#define RUVIA_REDIS_ENTITY(Name, Prefix, ...)                                               \
    struct Name final : ::ruvia::RedisEntity<::ruvia::FixedString{Prefix}, __VA_ARGS__> {   \
        using ::ruvia::RedisEntity<::ruvia::FixedString{Prefix}, __VA_ARGS__>::RedisEntity; \
    };

}  // namespace ruvia
