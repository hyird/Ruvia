#pragma once

#include <memory>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

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
        } else if constexpr (std::is_same_v<T, std::pmr::string> || IsPmrVector<T>::value) {
            return T(resource);
        } else {
            return T{};
        }
    }
    void clear() {
        // Even an empty container can allocate (for example, a debug iterator
        // proxy). Keep the current value alive until that construction succeeds.
        auto empty = make_value(resource_);
        static_assert(std::is_nothrow_move_constructible_v<T>);
        value.~T();
        std::construct_at(&value, std::move(empty));
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
        out.assignOwned(std::string_view(value));
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
            T owned(resource);
            owned.reserve(value.size());
            for (auto& item : value) {
                if constexpr (std::is_lvalue_reference_v<V>) {
                    owned.emplace_back(item);
                } else {
                    owned.emplace_back(std::move(item));
                }
            }
            out = std::move(owned);
        } else {
            T owned(resource);
            owned.reserve(value.size());
            for (const auto& item : value) {
                auto element = entity_value_slot<Element>::make_value(resource);
                assignEntityValue(element, item, resource);
                owned.push_back(std::move(element));
            }
            out = std::move(owned);
        }
    } else {
        out = std::forward<V>(value);
    }
}

}  // namespace ruvia::detail
