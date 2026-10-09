#pragma once

#include <cstddef>
#include <memory>
#include <memory_resource>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

#include "ruvia/web/attributes.h"
#include "ruvia/web/detail/model/model_schema.h"

namespace ruvia::detail::model {

template <typename... descriptor_types>
class model_storage final {
public:
    using schema_type = model_schema<descriptor_types...>;
    static constexpr bool has_initial =
        (descriptor_types::options_type::has_initial || ... || false);

    explicit model_storage(::ruvia::model_options options = {}) noexcept
        : resource_(detail::pmr_resource_or_default(options.resource_)) {
        static_assert(unique_model_field_names<descriptor_types...>(),
            "Ruvia model source field names must be unique");
        static_assert(
            unique_model_wire_names<descriptor_types...>(), "Ruvia model JSON field names must be unique");
    }

    model_storage(const model_storage&) = delete;
    model_storage& operator=(const model_storage&) = delete;

    model_storage(model_storage&& other) noexcept
        : resource_(other.resource_),
          fields_(std::move(other.fields_)) {
        reset_moved_fields(other.fields_);
    }

    friend bool operator==(const model_storage& left, const model_storage& right) {
        return left.fields_ == right.fields_;
    }

    model_storage& operator=(model_storage&& other) {
        if (this == &other) {
            return *this;
        }

        auto rebound = rebind_fields(other.fields_, resource_);
        reset_moved_fields(other.fields_);
        replace_fields(fields_, std::move(rebound));
        return *this;
    }

    template <fixed_string field>
    [[nodiscard]] decltype(auto) get() const& RUVIA_LIFETIMEBOUND {
        constexpr auto index = model_field_index<field, descriptor_types...>();
        using descriptor_type = std::tuple_element_t<index, std::tuple<descriptor_types...>>;
        const auto& slot = std::get<index>(fields_);
        if constexpr (descriptor_type::required && !descriptor_type::nullable) {
            return slot.required_value();
        } else {
            return slot.value();
        }
    }

    template <fixed_string field>
    [[nodiscard]] decltype(auto) get() const&& = delete;

    // Whether the original parsed input contained this field. Defaults and
    // set/ensure/reset do not rewrite input provenance; unparsed models have
    // no input presence. Use get()/is_null() for the current value state.
    template <fixed_string field>
    [[nodiscard]] bool is_present() const noexcept {
        constexpr auto index = model_field_index<field, descriptor_types...>();
        return std::get<index>(fields_).is_present();
    }

    template <fixed_string field>
    [[nodiscard]] bool is_null() const noexcept {
        constexpr auto index = model_field_index<field, descriptor_types...>();
        return std::get<index>(fields_).is_null();
    }

    template <fixed_string field, typename value_type>
    void set(value_type&& value) & {
        constexpr auto index = model_field_index<field, descriptor_types...>();
        std::get<index>(fields_).assign(std::forward<value_type>(value), resource_);
    }

    template <fixed_string field>
    [[nodiscard]] decltype(auto) ensure() & RUVIA_LIFETIMEBOUND {
        constexpr auto index = model_field_index<field, descriptor_types...>();
        return std::get<index>(fields_).ensure(resource_);
    }

    template <fixed_string field>
        requires(!std::tuple_element_t<model_field_index<field, descriptor_types...>(),
            std::tuple<descriptor_types...>>::required)
    void reset() & noexcept {
        constexpr auto index = model_field_index<field, descriptor_types...>();
        std::get<index>(fields_).reset();
    }

    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept {
        return resource_;
    }

    template <std::size_t index>
    [[nodiscard]] decltype(auto) slot() & noexcept {
        return std::get<index>(fields_);
    }

    template <std::size_t index>
    [[nodiscard]] decltype(auto) slot() const& noexcept {
        return std::get<index>(fields_);
    }

    [[nodiscard]] model_storage rebind(std::pmr::memory_resource* resource) const& {
        model_storage rebound(::ruvia::model_options{.resource_ = resource});
        auto fields_value = rebind_fields(fields_, rebound.resource_);
        replace_fields(rebound.fields_, std::move(fields_value));
        return rebound;
    }

    [[nodiscard]] model_storage rebind(std::pmr::memory_resource* resource) && {
        auto rebound = std::as_const(*this).rebind(resource);
        reset_moved_fields(fields_);
        return rebound;
    }

private:
    using field_tuple = std::tuple<typename descriptor_types::field_type...>;

    template <std::size_t... indices>
    [[nodiscard]] static field_tuple rebind_fields(const field_tuple& source_value,
        std::pmr::memory_resource* resource, std::index_sequence<indices...>) {
        return field_tuple{std::get<indices>(source_value).rebind(resource)...};
    }

    [[nodiscard]] static field_tuple rebind_fields(
        const field_tuple& source_value, std::pmr::memory_resource* resource) {
        return rebind_fields(source_value, resource, std::index_sequence_for<descriptor_types...>{});
    }

    static void replace_fields(field_tuple& destination, field_tuple&& source_value) noexcept {
        static_assert(std::is_nothrow_move_constructible_v<field_tuple>);
        std::destroy_at(std::addressof(destination));
        std::construct_at(std::addressof(destination), std::move(source_value));
    }

    template <std::size_t... indices>
    static void reset_moved_fields(field_tuple& fields_value, std::index_sequence<indices...>) noexcept {
        (std::get<indices>(fields_value).reset_after_move(), ...);
    }

    static void reset_moved_fields(field_tuple& fields_value) noexcept {
        reset_moved_fields(fields_value, std::index_sequence_for<descriptor_types...>{});
    }

    std::pmr::memory_resource* resource_;
    field_tuple fields_;
};

}  // namespace ruvia::detail::model
