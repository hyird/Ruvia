#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "ruvia/web/detail/json/json_escape.h"
#include "ruvia/web/detail/model/rule/rule_support.h"

namespace ruvia::detail::model {

// Field options are type-level metadata, not per-model runtime objects.
template <typename... option_types>
class model_options final {
    static_assert((is_model_option<option_types>() && ... && true),
        "model field options must be RUVIA_DEFAULT, RUVIA_INITIAL, RUVIA_NULLABLE, RUVIA_OMIT_EMPTY, or RUVIA_EMIT_NULL");
    static_assert(((is_default_rule<option_types>() ? 1 : 0) + ... + 0) <= 1,
        "a model field may have at most one RUVIA_DEFAULT");
    static_assert(((is_initial_rule<option_types>() ? 1 : 0) + ... + 0) <= 1,
        "a model field may have at most one RUVIA_INITIAL");

public:
    static constexpr bool nullable = (std::is_same_v<option_types, ::ruvia::detail::model::nullable> || ... || false);

    [[nodiscard]] static constexpr bool emit_null() noexcept {
        return contains_option<::ruvia::detail::model::emit_null>();
    }

    [[nodiscard]] static constexpr bool omit_empty() noexcept {
        return contains_option<::ruvia::detail::model::omit_empty>();
    }

    static constexpr bool has_initial = (is_initial_rule<option_types>() || ... || false);
    static constexpr bool has_default = (is_default_rule<option_types>() || ... || false);

    // The field owns assignment, nullability and allocator normalization.
    template <typename consumer_type>
    static void apply_default(consumer_type&& consume) {
        if constexpr (sizeof...(option_types) != 0) {
            (apply_default_option<option_types>(consume), ...);
        } else {
            (void)consume;
        }
    }

    template <typename consumer_type>
    static void apply_initial(consumer_type&& consume) {
        if constexpr (sizeof...(option_types) != 0) {
            (apply_initial_option<option_types>(consume), ...);
        } else {
            (void)consume;
        }
    }

private:
    template <typename option_type>
    [[nodiscard]] static constexpr bool contains_option() noexcept {
        return (std::is_same_v<std::remove_cvref_t<option_types>, option_type> || ... || false);
    }

    template <typename option_type, typename consumer_type>
    static void apply_initial_option(consumer_type& consume) {
        if constexpr (is_initial_rule<option_type>()) {
            consume(option_type::value());
        } else {
            (void)consume;
        }
    }

    template <typename option_type, typename consumer_type>
    static void apply_default_option(consumer_type& consume) {
        if constexpr (is_default_rule<option_type>()) {
            consume(option_type::value());
        } else {
            (void)consume;
        }
    }
};

template <typename field_type, typename value_type>
void assign_field_value(
    std::optional<field_type>& target, value_type&& value, std::pmr::memory_resource* resource) {
    if constexpr (detail::is_ruvia_scalar<field_type>) {
        // Validate narrowing before replacing an existing field value.
        const auto field = [&] {
            if constexpr (detail::is_ruvia_scalar<value_type>) {
                return field_type(value.value_);
            } else {
                return field_type(std::forward<value_type>(value));
            }
        }();
        target.emplace(field);
    } else if constexpr (detail::is_ruvia_string<field_type> &&
                         std::is_same_v<std::remove_cvref_t<value_type>, std::pmr::string> &&
                         std::is_rvalue_reference_v<value_type&&>) {
        field_type field(::ruvia::model_options{.resource_ = resource});
        field.assign_owned(std::forward<value_type>(value));
        target.emplace(std::move(field));
    } else if constexpr (detail::is_ruvia_string<field_type> &&
                         std::is_convertible_v<value_type&&, std::string_view> &&
                         !std::is_same_v<std::remove_cvref_t<value_type>, field_type>) {
        field_type field(::ruvia::model_options{.resource_ = resource});
        field.assign_owned(std::string_view(std::forward<value_type>(value)));
        target.emplace(std::move(field));
    } else if constexpr (detail::is_ruvia_bytes<field_type> &&
                         !std::is_same_v<std::remove_cvref_t<value_type>, field_type>) {
        field_type field(::ruvia::model_options{.resource_ = resource});
        if constexpr (std::is_same_v<std::remove_cvref_t<value_type>, std::pmr::vector<std::uint8_t>> &&
                      std::is_rvalue_reference_v<value_type&&>) {
            field.assign_owned(std::forward<value_type>(value));
        } else {
            field.assign_owned(std::span<const std::uint8_t>(std::forward<value_type>(value)));
        }
        target.emplace(std::move(field));
    } else {
        if constexpr (!std::is_lvalue_reference_v<value_type&&> &&
                      !std::is_const_v<std::remove_reference_t<value_type>>) {
            // Consuming a source that owns this field would destroy the destination.
            if (model_value_rebind_access::owns(value, std::addressof(target))) {
                throw_model_move_source_owns_destination();
            }
        }
        target.emplace(detail::rebind_model_value(std::forward<value_type>(value), resource));
    }
}

template <typename... descriptor_types>
class model_storage;

// Byte sinks for append_json_member_key: the first measures, the second fills
// an exactly sized compile-time buffer.
struct json_member_key_size_sink final {
    std::size_t size_{0};

    constexpr void push_back(char) noexcept {
        ++size_;
    }
    constexpr void append(std::string_view text) noexcept {
        size_ += text.size();
    }
};

template <std::size_t capacity>
struct json_member_key_buffer final {
    std::array<char, capacity> bytes_{};
    std::size_t size_{0};

    constexpr void push_back(char value) noexcept {
        bytes_[size_++] = value;
    }
    constexpr void append(std::string_view text) noexcept {
        for (const char value : text) {
            push_back(value);
        }
    }
};

// Size of `,"<escaped wire name>":`, or 0 when the name is not well-formed UTF-8.
template <fixed_string wire_name>
[[nodiscard]] consteval std::size_t json_member_key_size() {
    json_member_key_size_sink sink;
    return append_json_member_key(sink, wire_name.view()) ? sink.size_ : 0;
}

template <fixed_string wire_name>
[[nodiscard]] consteval std::array<char, json_member_key_size<wire_name>()> make_json_member_key() {
    json_member_key_buffer<json_member_key_size<wire_name>()> key;
    (void)append_json_member_key(key, wire_name.view());
    return key.bytes_;
}

template <typename field_value_type, bool is_required, typename options_type, fixed_string field_wire_name>
class model_field final {
    static_assert(std::is_empty_v<options_type>, "model field options must be stateless metadata");
    static_assert(json_member_key_size<field_wire_name>() != 0, "Ruvia model wire names must be well-formed UTF-8");
    // A default never satisfies a required input field, so accepting one would
    // silently ignore it.
    static_assert(!(is_required && options_type::has_default),
        "RUVIA_DEFAULT applies only to a missing optional field; declare it with RUVIA_OPTIONAL_FIELD");

public:
    using value_type = field_value_type;
    static constexpr bool required = is_required;
    static constexpr bool nullable = options_type::nullable;
    constexpr model_field() noexcept = default;

    [[nodiscard]] constexpr std::string_view wire_name() const noexcept {
        return field_wire_name.view();
    }

    // `,"<wire name>":` escaped at compile time; the first emitted member drops the comma.
    [[nodiscard]] static constexpr std::string_view json_member_key(bool leading_comma) noexcept {
        const std::string_view key(json_member_key_.data(), json_member_key_.size());
        return leading_comma ? key : key.substr(1);
    }

    [[nodiscard]] detail::model_field_state state() const noexcept {
        return state_;
    }

    // Input provenance survives defaults and later application mutations.
    [[nodiscard]] bool is_present() const noexcept {
        return present_;
    }

    [[nodiscard]] bool is_null() const noexcept {
        return state_ == detail::model_field_state::null;
    }

    [[nodiscard]] bool has_value() const noexcept {
        return value_.has_value();
    }

    [[nodiscard]] const std::optional<field_value_type>& value() const& noexcept RUVIA_LIFETIMEBOUND {
        return value_;
    }

    [[nodiscard]] const std::optional<field_value_type>& value() const&& = delete;

    [[nodiscard]] const field_value_type& required_value() const& RUVIA_LIFETIMEBOUND
        requires required
    {
        if (!value_) {
            throw std::logic_error("required model field has no value");
        }
        return *value_;
    }

    [[nodiscard]] const field_value_type& required_value() const&&
        requires required
    = delete;

    // Dynamic JSON tokens require whole-value assignment to keep null state
    // synchronized; only values that cannot become JSON null expose mutation.
    [[nodiscard]] field_value_type& ensure(std::pmr::memory_resource* resource) & RUVIA_LIFETIMEBOUND
        requires(!detail::is_ruvia_json_value<field_value_type>)
    {
        if (!value_) {
            value_.emplace(
                detail::make_request_value<field_value_type>(resolved_pmr_resource_tag{}, resource));
        }
        state_ = detail::model_field_state::parsed;
        return *value_;
    }

    field_value_type& ensure(std::pmr::memory_resource*) &&
        requires(!detail::is_ruvia_json_value<field_value_type>)
    = delete;

    template <typename input_type>
    void assign(input_type&& input, std::pmr::memory_resource* resource) {
        if constexpr (std::is_null_pointer_v<std::remove_cvref_t<input_type>>) {
            static_assert(nullable, "setting null requires RUVIA_NULLABLE");
            assign_null();
        } else {
            if constexpr (detail::is_ruvia_json_value<input_type>) {
                if (input.is_null()) {
                    if constexpr (nullable) {
                        assign_null();
                        return;
                    } else {
                        throw std::invalid_argument("setting JSON null requires RUVIA_NULLABLE");
                    }
                }
            }
            assign_field_value(value_, std::forward<input_type>(input), resource);
            state_ = detail::model_field_state::parsed;
        }
    }

    void set_null() noexcept
        requires nullable
    {
        assign_null();
    }

    void reset() noexcept {
        state_ = detail::model_field_state::missing;
        value_.reset();
    }

    void apply_initial(std::pmr::memory_resource* resource) {
        options_type::apply_initial([this, resource]<typename input_type>(input_type&& value) {
            this->assign(std::forward<input_type>(value), resource);
        });
    }

    void apply_default(std::pmr::memory_resource* resource) {
        if constexpr (options_type::has_default) {
            if (state_ != detail::model_field_state::missing) {
                return;
            }
            options_type::apply_default([this, resource]<typename input_type>(input_type&& value) {
                this->assign(std::forward<input_type>(value), resource);
            });
        } else {
            (void)resource;
        }
    }

    [[nodiscard]] constexpr bool emit_null() const noexcept {
        return options_type::emit_null();
    }

    [[nodiscard]] constexpr bool omit_empty() const noexcept {
        return options_type::omit_empty();
    }

    friend bool operator==(const model_field& left, const model_field& right) {
        if (left.state_ != right.state_) {
            return false;
        }
        if (left.state_ == detail::model_field_state::parsed) {
            return left.value_ == right.value_;
        }
        return true;
    }

    void mark_duplicate() noexcept {
        present_ = true;
        state_ = detail::model_field_state::duplicate;
    }

    void mark_invalid_type() noexcept {
        present_ = true;
        state_ = detail::model_field_state::invalid_type;
    }

    void mark_null() noexcept {
        present_ = true;
        assign_null();
    }

private:
    template <typename... descriptor_types>
    friend class model_storage;
    friend struct ::ruvia::detail::model_value_factory;

    void assign_null() noexcept {
        value_.reset();
        state_ = detail::model_field_state::null;
    }

    void emplace_parsed(field_value_type&& value) {
        value_.emplace(std::move(value));
        present_ = true;
        state_ = detail::model_field_state::parsed;
    }

    [[nodiscard]] model_field rebind(std::pmr::memory_resource* resource) const {
        model_field rebound;
        rebound.state_ = state_;
        rebound.present_ = present_;
        if (value_) {
            rebound.value_.emplace(detail::rebind_model_value(*value_, resource));
        }
        return rebound;
    }

    [[nodiscard]] bool owns(const void* address) const noexcept {
        return value_ && model_value_rebind_access::owns(*value_, address);
    }

    void reset_after_move() noexcept {
        present_ = false;
        value_.reset();
        state_ = detail::model_field_state::missing;
    }

    static constexpr auto json_member_key_ = make_json_member_key<field_wire_name>();
    detail::model_field_state state_{detail::model_field_state::missing};
    bool present_{false};
    std::optional<field_value_type> value_;
};

}  // namespace ruvia::detail::model
