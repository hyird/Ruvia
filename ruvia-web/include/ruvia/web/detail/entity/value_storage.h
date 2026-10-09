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

#include "ruvia/core/memory/pmr_resource.h"
#include "ruvia/web/fixed_string.h"
#include "ruvia/web/model_types.h"

namespace ruvia::detail {

// Shared owning value storage. Backend descriptors and relations stay with
// their backend; each slot normalizes data into its entity's PMR resource.
template <typename t_type>
struct is_pmr_vector : std::false_type {};
template <typename t_type>
struct is_pmr_vector<std::pmr::vector<t_type>> : std::true_type {
    using value_type = t_type;
};

template <typename value_type>
void commit_entity_value(value_type& target, value_type&& owned) noexcept {
    if constexpr (std::is_same_v<value_type, std::pmr::string> || is_pmr_vector<value_type>::value) {
        // Both values belong to the same resource. Swap also avoids allocating
        // debug iterator proxies during publication.
        target.swap(owned);
    } else {
        static_assert(std::is_nothrow_move_constructible_v<value_type>);
        target.~value_type();
        std::construct_at(&target, std::move(owned));
    }
}
template <typename t_type>
struct entity_value_slot {
    enum class state_type : unsigned char { unset,
        null,
        value };
    explicit entity_value_slot(std::pmr::memory_resource* resource)
        : resource_(resource),
          value_(make_value(resource)) {}
    static t_type make_value(std::pmr::memory_resource* resource) {
        if constexpr (std::is_same_v<t_type, string>) {
            return string(model_options{.resource_ = resource});
        } else if constexpr (std::is_same_v<t_type, std::pmr::string>) {
            // MSVC Debug allocates iterator proxies even for empty containers.
            // Use constructors that can propagate an allocation failure.
            return t_type(0, '\0', resource);
        } else if constexpr (is_pmr_vector<t_type>::value) {
            return t_type(0, resource);
        } else {
            return t_type{};
        }
    }
    void clear() {
        // Even an empty container can allocate (for example, a debug iterator
        // proxy). Keep the current value alive until that construction succeeds.
        auto empty = make_value(resource_);
        commit_entity_value(value_, std::move(empty));
    }
    std::pmr::memory_resource* resource_{nullptr};
    state_type state_{state_type::unset};
    t_type value_;
};

template <fixed_string name, typename... column_types_type>
consteval std::size_t entity_column_index() {
    constexpr bool matches[] = {column_types_type::name == name...};
    for (std::size_t i = 0; i < sizeof...(column_types_type); ++i) {
        if (matches[i]) {
            return i;
        }
    }
    return sizeof...(column_types_type);
}
template <typename... column_types_type>
consteval bool unique_entity_columns() {
    constexpr std::string_view names[] = {column_types_type::name.view()...};
    for (std::size_t i = 0; i < sizeof...(column_types_type); ++i) {
        for (std::size_t j = i + 1; j < sizeof...(column_types_type); ++j) {
            if (names[i] == names[j]) {
                return false;
            }
        }
    }
    return true;
}

template <typename t_type>
struct is_optional : std::false_type {};
template <typename t_type>
struct is_optional<std::optional<t_type>> : std::true_type {
    using value_type = t_type;
};

template <typename t_type, typename v_type>
void assign_entity_value(t_type& out, v_type&& value, std::pmr::memory_resource* resource) {
    if constexpr (std::is_same_v<t_type, string>) {
        auto owned = string(std::string_view(value), model_options{.resource_ = resource});
        commit_entity_value(out, std::move(owned));
    } else if constexpr (std::is_same_v<t_type, std::pmr::string>) {
        out = std::forward<v_type>(value);
    } else if constexpr (is_optional<t_type>::value) {
        if (!value) {
            out.reset();
        } else {
            using element_type = typename is_optional<t_type>::value_type;
            auto owned = entity_value_slot<element_type>::make_value(resource);
            assign_entity_value(owned, *value, resource);
            out.emplace(std::move(owned));
        }
    } else if constexpr (is_pmr_vector<t_type>::value) {
        using element_type = typename is_pmr_vector<t_type>::value_type;
        if constexpr (std::is_arithmetic_v<element_type>) {
            out = std::forward<v_type>(value);
        } else if constexpr (std::is_same_v<element_type, std::pmr::string>) {
            auto owned = entity_value_slot<t_type>::make_value(resource);
            owned.reserve(value.size());
            for (auto& item : value) {
                if constexpr (std::is_lvalue_reference_v<v_type>) {
                    owned.emplace_back(item);
                } else {
                    owned.emplace_back(std::move(item));
                }
            }
            commit_entity_value(out, std::move(owned));
        } else {
            auto owned = entity_value_slot<t_type>::make_value(resource);
            owned.reserve(value.size());
            for (const auto& item : value) {
                auto element = entity_value_slot<element_type>::make_value(resource);
                assign_entity_value(element, item, resource);
                owned.push_back(std::move(element));
            }
            commit_entity_value(out, std::move(owned));
        }
    } else {
        if constexpr (std::is_nothrow_assignable_v<t_type&, v_type&&>) {
            out = std::forward<v_type>(value);
        } else {
            auto owned = entity_value_slot<t_type>::make_value(resource);
            owned = std::forward<v_type>(value);
            commit_entity_value(out, std::move(owned));
        }
    }
}

template <typename... columns>
class entity_value_storage final {
    static_assert(unique_entity_columns<columns...>(), "duplicate entity column name");
    using slots_type = std::tuple<entity_value_slot<typename columns::value_type>...>;
    template <fixed_string name>
    auto& slot() {
        return std::get<index<name>()>(slots_);
    }
    template <fixed_string name>
    const auto& slot() const {
        return std::get<index<name>()>(slots_);
    }

public:
    using columns_type = std::tuple<columns...>;
    template <fixed_string name>
    static consteval std::size_t index() {
        constexpr auto result_value = entity_column_index<name, columns...>();
        static_assert(result_value < sizeof...(columns), "unknown entity column");
        return result_value;
    }
    explicit entity_value_storage(std::pmr::memory_resource* resource = nullptr)
        : resource_(pmr_resource_or_default(resource)),
          slots_(entity_value_slot<typename columns::value_type>(resource_)...) {}
    entity_value_storage(const entity_value_storage&) = delete;
    entity_value_storage& operator=(const entity_value_storage&) = delete;
    entity_value_storage(entity_value_storage&&) noexcept = default;
    entity_value_storage& operator=(entity_value_storage&&) = delete;
    template <fixed_string name>
    auto& get() & {
        auto& value = slot<name>();
        if (value.state_ != decltype(value.state_)::value) {
            throw std::logic_error("entity value is not set");
        }
        return value.value_;
    }
    template <fixed_string name>
    const auto& get() const& {
        return const_cast<entity_value_storage*>(this)->template get<name>();
    }
    template <fixed_string name>
    const auto& get() const&& = delete;
    template <fixed_string name, typename value_type>
    void set(value_type&& value) {
        auto& target = slot<name>();
        assign_entity_value(target.value_, std::forward<value_type>(value), resource_);
        target.state_ = decltype(target.state_)::value;
    }
    template <fixed_string name>
    void set_null()
        requires(std::tuple_element_t<index<name>(), columns_type>::options.nullable_)
    {
        auto& value = slot<name>();
        value.clear();
        value.state_ = decltype(value.state_)::null;
    }
    template <fixed_string name>
    void reset() {
        auto& value = slot<name>();
        value.clear();
        value.state_ = decltype(value.state_)::unset;
    }
    template <fixed_string name>
    bool is_set() const {
        const auto& value = slot<name>();
        return value.state_ != decltype(value.state_)::unset;
    }
    template <fixed_string name>
    bool is_null() const {
        const auto& value = slot<name>();
        return value.state_ == decltype(value.state_)::null;
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
    template <::ruvia::fixed_string name>                                              \
    auto& get() & {                                                                    \
        return member.template get<name>();                                            \
    }                                                                                  \
    template <::ruvia::fixed_string name>                                              \
    const auto& get() const& {                                                         \
        return member.template get<name>();                                            \
    }                                                                                  \
    template <::ruvia::fixed_string name>                                              \
    const auto& get() const&& = delete;                                                \
    template <::ruvia::fixed_string name, typename value_type>                         \
    void set(value_type&& value) {                                                     \
        member.template set<name>(std::forward<value_type>(value));                    \
    }                                                                                  \
    template <::ruvia::fixed_string name>                                              \
    void set_null()                                                                    \
        requires requires { member.template null_method<name>(); }                     \
    {                                                                                  \
        member.template null_method<name>();                                           \
    }                                                                                  \
    template <::ruvia::fixed_string name>                                              \
    void reset() {                                                                     \
        member.template reset<name>();                                                 \
    }                                                                                  \
    template <::ruvia::fixed_string name>                                              \
    bool is_set() const {                                                              \
        return member.template set_method<name>();                                     \
    }                                                                                  \
    template <::ruvia::fixed_string name>                                              \
    bool is_null() const {                                                             \
        return member.template is_null_method<name>();                                 \
    }                                                                                  \
    std::pmr::memory_resource* resource() const noexcept {                             \
        return member.resource();                                                      \
    }
