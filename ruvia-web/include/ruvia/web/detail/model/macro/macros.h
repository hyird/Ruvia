#pragma once

#include <concepts>
#include <memory_resource>
#include <utility>

#include "ruvia/web/attributes.h"
#include "ruvia/web/detail/model/macro/macro_field_ops.h"
#include "ruvia/web/detail/model/model_storage.h"
#include "ruvia/web/fixed_string.h"

// Field descriptors are forwarded directly into a C++ variadic template. No
// preprocessor argument counter or FOR_EACH expansion participates in model
// registration, so the framework does not impose a fixed field-count limit.
// Each declaration is a nominal type, including forward-declared recursive
// schemas. Its inline field owner does not know the enclosing model type.

#define RUVIA_MODEL(t_type, ...)                                                                           \
    struct t_type final {                                                                                  \
    private:                                                                                               \
        using storage_type = ::ruvia::detail::model::model_storage<__VA_ARGS__>;                           \
                                                                                                           \
    public:                                                                                                \
        using ruvia_model_schema_type = ::ruvia::detail::model::model_schema<__VA_ARGS__>;                 \
        template <typename model_type = t_type>                                                            \
        explicit t_type(::ruvia::model_options options = {}) noexcept(!storage_type::has_initial)          \
            : fields_(options) {                                                                           \
            ::ruvia::detail::model::validate_model_schema(typename model_type::ruvia_model_schema_type{}); \
            if constexpr (storage_type::has_initial) {                                                     \
                ::ruvia::detail::model::initialize_model(*this);                                           \
            }                                                                                              \
        }                                                                                                  \
        template <typename resource_owner_type>                                                            \
            requires requires(resource_owner_type& owner) {                                                \
                { owner.resource() } -> std::convertible_to<std::pmr::memory_resource*>;                   \
            }                                                                                              \
        explicit t_type(resource_owner_type& owner) noexcept(!storage_type::has_initial)                   \
            : t_type(::ruvia::model_options{.resource_ = owner.resource()}) {}                             \
        t_type(const t_type&) = delete;                                                                    \
        t_type& operator=(const t_type&) = delete;                                                         \
        t_type(t_type&&) noexcept = default;                                                               \
        t_type& operator=(t_type&&) = default;                                                             \
        friend bool operator==(const t_type&, const t_type&) = default;                                    \
        template <::ruvia::fixed_string field>                                                             \
        [[nodiscard]] decltype(auto) get() const& RUVIA_LIFETIMEBOUND {                                    \
            return fields_.template get<field>();                                                          \
        }                                                                                                  \
        template <::ruvia::fixed_string field>                                                             \
        [[nodiscard]] decltype(auto) get() const&& = delete;                                               \
        template <::ruvia::fixed_string field>                                                             \
        [[nodiscard]] bool is_present() const noexcept {                                                   \
            return fields_.template is_present<field>();                                                   \
        }                                                                                                  \
        template <::ruvia::fixed_string field>                                                             \
        [[nodiscard]] bool is_null() const noexcept {                                                      \
            return fields_.template is_null<field>();                                                      \
        }                                                                                                  \
        template <::ruvia::fixed_string field, typename value_type>                                        \
        t_type& set(value_type&& value) & {                                                                \
            fields_.template set<field>(std::forward<value_type>(value));                                  \
            return *this;                                                                                  \
        }                                                                                                  \
        template <::ruvia::fixed_string field, typename value_type>                                        \
        t_type& set(value_type&&) && = delete;                                                             \
        template <::ruvia::fixed_string field>                                                             \
        [[nodiscard]] decltype(auto) ensure() & RUVIA_LIFETIMEBOUND {                                      \
            return fields_.template ensure<field>();                                                       \
        }                                                                                                  \
        template <::ruvia::fixed_string field>                                                             \
        [[nodiscard]] decltype(auto) ensure() && = delete;                                                 \
        template <::ruvia::fixed_string field>                                                             \
            requires requires(storage_type& storage) { storage.template reset<field>(); }                  \
        void reset() & noexcept {                                                                          \
            fields_.template reset<field>();                                                               \
        }                                                                                                  \
        template <::ruvia::fixed_string field>                                                             \
            requires requires(storage_type& storage) { storage.template reset<field>(); }                  \
        void reset() && = delete;                                                                          \
        [[nodiscard]] std::pmr::memory_resource* resource() const noexcept {                               \
            return fields_.resource();                                                                     \
        }                                                                                                  \
                                                                                                           \
    private:                                                                                               \
        friend struct ::ruvia::detail::model::model_access;                                                \
        friend struct ::ruvia::detail::model_value_rebind_access;                                          \
        template <typename model_type = t_type>                                                            \
        explicit t_type(::ruvia::detail::model::empty_model_tag, ::ruvia::model_options options) noexcept  \
            : fields_(options) {                                                                           \
            ::ruvia::detail::model::validate_model_schema(typename model_type::ruvia_model_schema_type{}); \
        }                                                                                                  \
        explicit t_type(storage_type&& fields) noexcept                                                    \
            : fields_(std::move(fields)) {}                                                                \
        storage_type fields_;                                                                              \
    }
