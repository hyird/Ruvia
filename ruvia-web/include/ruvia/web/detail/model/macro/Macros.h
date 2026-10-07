#pragma once

#include <concepts>
#include <memory_resource>
#include <utility>

#include "ruvia/web/Attributes.h"
#include "ruvia/web/FixedString.h"
#include "ruvia/web/detail/model/macro/MacroFieldOps.h"
#include "ruvia/web/detail/model/model_storage.h"

// Field descriptors are forwarded directly into a C++ variadic template. No
// preprocessor argument counter or FOR_EACH expansion participates in model
// registration, so the framework does not impose a fixed field-count limit.
// Each declaration is a nominal type, including forward-declared recursive
// schemas. Its inline field owner does not know the enclosing model type.

#define RUVIA_MODEL(T, ...)                                                                         \
    struct T final {                                                                                \
    private:                                                                                        \
        using storage_type = ::ruvia::detail::model::model_storage<__VA_ARGS__>;                    \
                                                                                                    \
    public:                                                                                         \
        using RuviaModelSchema = ::ruvia::detail::model::ModelSchema<__VA_ARGS__>;                  \
        template <typename model_type = T>                                                          \
        explicit T(::ruvia::ModelOptions options = {}) noexcept(!storage_type::has_initial)         \
            : fields_(options) {                                                                    \
            ::ruvia::detail::model::validate_model_schema(typename model_type::RuviaModelSchema{}); \
            if constexpr (storage_type::has_initial) {                                              \
                ::ruvia::detail::model::initialize_model(*this);                                    \
            }                                                                                       \
        }                                                                                           \
        template <typename resource_owner_type>                                                     \
            requires requires(resource_owner_type& owner) {                                         \
                { owner.resource() } -> std::convertible_to<std::pmr::memory_resource*>;            \
            }                                                                                       \
        explicit T(resource_owner_type& owner) noexcept(!storage_type::has_initial)                 \
            : T(::ruvia::ModelOptions{.resource = owner.resource()}) {}                             \
        T(const T&) = delete;                                                                       \
        T& operator=(const T&) = delete;                                                            \
        T(T&&) noexcept = default;                                                                  \
        T& operator=(T&&) = default;                                                                \
        friend bool operator==(const T&, const T&) = default;                                       \
        template <::ruvia::FixedString field>                                                       \
        [[nodiscard]] decltype(auto) get() const& RUVIA_LIFETIMEBOUND {                             \
            return fields_.template get<field>();                                                   \
        }                                                                                           \
        template <::ruvia::FixedString field>                                                       \
        [[nodiscard]] decltype(auto) get() const&& = delete;                                        \
        template <::ruvia::FixedString field>                                                       \
        [[nodiscard]] bool isPresent() const noexcept {                                             \
            return fields_.template is_present<field>();                                            \
        }                                                                                           \
        template <::ruvia::FixedString field>                                                       \
        [[nodiscard]] bool isNull() const noexcept {                                                \
            return fields_.template is_null<field>();                                               \
        }                                                                                           \
        template <::ruvia::FixedString field, typename value_type>                                  \
        T& set(value_type&& value) & {                                                              \
            fields_.template set<field>(std::forward<value_type>(value));                           \
            return *this;                                                                           \
        }                                                                                           \
        template <::ruvia::FixedString field, typename value_type>                                  \
        T& set(value_type&&) && = delete;                                                           \
        template <::ruvia::FixedString field>                                                       \
        [[nodiscard]] decltype(auto) ensure() & RUVIA_LIFETIMEBOUND {                               \
            return fields_.template ensure<field>();                                                \
        }                                                                                           \
        template <::ruvia::FixedString field>                                                       \
        [[nodiscard]] decltype(auto) ensure() && = delete;                                          \
        template <::ruvia::FixedString field>                                                       \
            requires requires(storage_type& storage) { storage.template reset<field>(); }           \
        void reset() & noexcept {                                                                   \
            fields_.template reset<field>();                                                        \
        }                                                                                           \
        template <::ruvia::FixedString field>                                                       \
            requires requires(storage_type& storage) { storage.template reset<field>(); }           \
        void reset() && = delete;                                                                   \
        [[nodiscard]] std::pmr::memory_resource* resource() const noexcept {                        \
            return fields_.resource();                                                              \
        }                                                                                           \
                                                                                                    \
    private:                                                                                        \
        friend struct ::ruvia::detail::model::model_access;                                         \
        friend struct ::ruvia::detail::ModelValueRebindAccess;                                      \
        template <typename model_type = T>                                                          \
        explicit T(::ruvia::detail::model::empty_model_tag, ::ruvia::ModelOptions options) noexcept \
            : fields_(options) {                                                                    \
            ::ruvia::detail::model::validate_model_schema(typename model_type::RuviaModelSchema{}); \
        }                                                                                           \
        explicit T(storage_type&& fields) noexcept                                                  \
            : fields_(std::move(fields)) {}                                                         \
        storage_type fields_;                                                                       \
    }
