#pragma once

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

#include "ruvia/core/memory/PmrResource.h"
#include "ruvia/web/FixedString.h"
#include "ruvia/web/ModelTypes.h"

namespace ruvia::detail {

// Shared owning value storage. Backend descriptors and relations stay with
// their backend; each slot normalizes data into its entity's PMR resource.
template <typename T>
struct IsPmrVector : std::false_type {};
template <typename T>
struct IsPmrVector<std::pmr::vector<T>> : std::true_type {
    using value_type = T;
};

template <typename value_type>
void commit_entity_value(value_type& target, value_type&& owned) noexcept {
    if constexpr (std::is_same_v<value_type, std::pmr::string> || IsPmrVector<value_type>::value) {
        // Both values belong to the same resource. Swap also avoids allocating
        // debug iterator proxies during publication.
        target.swap(owned);
    } else {
        static_assert(std::is_nothrow_move_constructible_v<value_type>);
        target.~value_type();
        std::construct_at(&target, std::move(owned));
    }
}
template <typename T>
struct entity_value_slot {
    enum class state_type : unsigned char { unset,
        null,
        value };
    explicit entity_value_slot(std::pmr::memory_resource* resource)
        : resource_(resource),
          value(make_value(resource)) {}
    static T make_value(std::pmr::memory_resource* resource) {
        if constexpr (std::is_same_v<T, String>) {
            return String(ModelOptions{.resource = resource});
        } else if constexpr (std::is_same_v<T, std::pmr::string>) {
            // MSVC Debug allocates iterator proxies even for empty containers.
            // Use constructors that can propagate an allocation failure.
            return T(0, '\0', resource);
        } else if constexpr (IsPmrVector<T>::value) {
            return T(0, resource);
        } else {
            return T{};
        }
    }
    void clear() {
        // Even an empty container can allocate (for example, a debug iterator
        // proxy). Keep the current value alive until that construction succeeds.
        auto empty = make_value(resource_);
        commit_entity_value(value, std::move(empty));
    }
    std::pmr::memory_resource* resource_{nullptr};
    state_type state{state_type::unset};
    T value;
};

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
        auto owned = String(std::string_view(value), ModelOptions{.resource = resource});
        commit_entity_value(out, std::move(owned));
    } else if constexpr (std::is_same_v<T, std::pmr::string>) {
        out = std::forward<V>(value);
    } else if constexpr (is_optional<T>::value) {
        if (!value) {
            out.reset();
        } else {
            using Element = typename is_optional<T>::value_type;
            auto owned = entity_value_slot<Element>::make_value(resource);
            assignEntityValue(owned, *value, resource);
            out.emplace(std::move(owned));
        }
    } else if constexpr (IsPmrVector<T>::value) {
        using Element = typename IsPmrVector<T>::value_type;
        if constexpr (std::is_arithmetic_v<Element>) {
            out = std::forward<V>(value);
        } else if constexpr (std::is_same_v<Element, std::pmr::string>) {
            auto owned = entity_value_slot<T>::make_value(resource);
            owned.reserve(value.size());
            for (auto& item : value) {
                if constexpr (std::is_lvalue_reference_v<V>) {
                    owned.emplace_back(item);
                } else {
                    owned.emplace_back(std::move(item));
                }
            }
            commit_entity_value(out, std::move(owned));
        } else {
            auto owned = entity_value_slot<T>::make_value(resource);
            owned.reserve(value.size());
            for (const auto& item : value) {
                auto element = entity_value_slot<Element>::make_value(resource);
                assignEntityValue(element, item, resource);
                owned.push_back(std::move(element));
            }
            commit_entity_value(out, std::move(owned));
        }
    } else {
        if constexpr (std::is_nothrow_assignable_v<T&, V&&>) {
            out = std::forward<V>(value);
        } else {
            auto owned = entity_value_slot<T>::make_value(resource);
            owned = std::forward<V>(value);
            commit_entity_value(out, std::move(owned));
        }
    }
}

template <typename... columns>
class entity_value_storage final {
    static_assert(uniqueEntityColumns<columns...>(), "duplicate entity column name");
    using slots_type = std::tuple<entity_value_slot<typename columns::value_type>...>;
    template <FixedString name>
    auto& slot() {
        return std::get<index<name>()>(slots_);
    }
    template <FixedString name>
    const auto& slot() const {
        return std::get<index<name>()>(slots_);
    }

public:
    using columns_type = std::tuple<columns...>;
    template <FixedString name>
    static consteval std::size_t index() {
        constexpr auto result = entityColumnIndex<name, columns...>();
        static_assert(result < sizeof...(columns), "unknown entity column");
        return result;
    }
    explicit entity_value_storage(std::pmr::memory_resource* resource = nullptr)
        : resource_(pmrResourceOrDefault(resource)),
          slots_(entity_value_slot<typename columns::value_type>(resource_)...) {}
    entity_value_storage(const entity_value_storage&) = delete;
    entity_value_storage& operator=(const entity_value_storage&) = delete;
    entity_value_storage(entity_value_storage&&) noexcept = default;
    entity_value_storage& operator=(entity_value_storage&&) = delete;
    template <FixedString name>
    auto& get() & {
        auto& value = slot<name>();
        if (value.state != decltype(value.state)::value) {
            throw std::logic_error("entity value is not set");
        }
        return value.value;
    }
    template <FixedString name>
    const auto& get() const& {
        return const_cast<entity_value_storage*>(this)->template get<name>();
    }
    template <FixedString name>
    const auto& get() const&& = delete;
    template <FixedString name, typename value_type>
    void set(value_type&& value) {
        auto& target = slot<name>();
        assignEntityValue(target.value, std::forward<value_type>(value), resource_);
        target.state = decltype(target.state)::value;
    }
    template <FixedString name>
    void set_null()
        requires(std::tuple_element_t<index<name>(), columns_type>::options.nullable)
    {
        auto& value = slot<name>();
        value.clear();
        value.state = decltype(value.state)::null;
    }
    template <FixedString name>
    void reset() {
        auto& value = slot<name>();
        value.clear();
        value.state = decltype(value.state)::unset;
    }
    template <FixedString name>
    bool is_set() const {
        const auto& value = slot<name>();
        return value.state != decltype(value.state)::unset;
    }
    template <FixedString name>
    bool is_null() const {
        const auto& value = slot<name>();
        return value.state == decltype(value.state)::null;
    }
    std::pmr::memory_resource* resource() const noexcept {
        return resource_;
    }

private:
    std::pmr::memory_resource* resource_;
    slots_type slots_;
};

template <typename tuple>
struct entity_storage_from_tuple;
template <typename... columns>
struct entity_storage_from_tuple<std::tuple<columns...>> {
    using type = entity_value_storage<columns...>;
};

}  // namespace ruvia::detail

// Public value facades only forward; state transitions belong to the owner.
#define RUVIA_DETAIL_ENTITY_VALUE_API(member, null_method, set_method, is_null_method) \
    template <::ruvia::FixedString name>                                               \
    auto& get() & {                                                                    \
        return member.template get<name>();                                            \
    }                                                                                  \
    template <::ruvia::FixedString name>                                               \
    const auto& get() const& {                                                         \
        return member.template get<name>();                                            \
    }                                                                                  \
    template <::ruvia::FixedString name>                                               \
    const auto& get() const&& = delete;                                                \
    template <::ruvia::FixedString name, typename value_type>                          \
    void set(value_type&& value) {                                                     \
        member.template set<name>(std::forward<value_type>(value));                    \
    }                                                                                  \
    template <::ruvia::FixedString name>                                               \
    void setNull()                                                                     \
        requires requires { member.template null_method<name>(); }                     \
    {                                                                                  \
        member.template null_method<name>();                                           \
    }                                                                                  \
    template <::ruvia::FixedString name>                                               \
    void reset() {                                                                     \
        member.template reset<name>();                                                 \
    }                                                                                  \
    template <::ruvia::FixedString name>                                               \
    bool isSet() const {                                                               \
        return member.template set_method<name>();                                     \
    }                                                                                  \
    template <::ruvia::FixedString name>                                               \
    bool isNull() const {                                                              \
        return member.template is_null_method<name>();                                 \
    }                                                                                  \
    std::pmr::memory_resource* resource() const noexcept {                             \
        return member.resource();                                                      \
    }
