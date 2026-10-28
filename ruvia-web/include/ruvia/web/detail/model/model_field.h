#pragma once

#include <array>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/web/detail/model/model_options.h"
#include "ruvia/web/detail/model/parse/field_assign.h"
#include "ruvia/web/detail/util/fnv1a.h"

namespace ruvia::detail::model {

template <typename... descriptor_types>
class model_storage;

[[nodiscard]] constexpr std::uint64_t model_field_name_hash(std::string_view name) noexcept {
    // FNV-1a is only a dispatch prefilter. The parser still compares the full
    // decoded key before binding, so collisions cannot change JSON semantics.
    return detail::fnv1a_accumulate(detail::fnv1a_offset_basis, name);
}
template <fixed_string wire_name>
[[nodiscard]] consteval std::array<char, wire_name.view().size() + 3>
make_quoted_wire_name_colon() noexcept {
    constexpr auto view = wire_name.view();
    std::array<char, view.size() + 3> out{};
    out[0] = '"';
    for (std::size_t i = 0; i < view.size(); ++i) {
        out[i + 1] = view[i];
    }
    out[view.size() + 1] = '"';
    out[view.size() + 2] = ':';
    return out;
}

template <fixed_string wire_name>
[[nodiscard]] consteval std::array<char, wire_name.view().size() + 4>
make_comma_quoted_wire_name_colon() noexcept {
    constexpr auto view = wire_name.view();
    std::array<char, view.size() + 4> out{};
    out[0] = ',';
    out[1] = '"';
    for (std::size_t i = 0; i < view.size(); ++i) {
        out[i + 2] = view[i];
    }
    out[view.size() + 2] = '"';
    out[view.size() + 3] = ':';
    return out;
}

template <typename field_value_type, bool is_required, typename options_type, fixed_string field_wire_name>
class model_field final {
    static_assert(std::is_empty_v<options_type>, "model field options must be stateless metadata");

public:
    using value_type = field_value_type;
    static constexpr bool required = is_required;
    static constexpr bool nullable = options_type::nullable;
    constexpr model_field() noexcept = default;

    [[nodiscard]] constexpr std::string_view wire_name() const noexcept {
        return field_wire_name.view();
    }
    [[nodiscard]] static consteval bool has_plain_wire_name() noexcept {
        for (const char ch : field_wire_name.view()) {
            const auto byte = static_cast<unsigned char>(ch);
            if (byte < 0x20 || byte >= 0x80 || ch == '"' || ch == '\\') {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] static constexpr std::string_view quoted_wire_name_colon() noexcept {
        return std::string_view(
            quoted_wire_name_colon_storage_.data(), quoted_wire_name_colon_storage_.size());
    }

    [[nodiscard]] static constexpr std::string_view comma_quoted_wire_name_colon() noexcept {
        return std::string_view(comma_quoted_wire_name_colon_storage_.data(),
            comma_quoted_wire_name_colon_storage_.size());
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

    [[nodiscard]] const std::optional<field_value_type>& value() const& noexcept {
        return value_;
    }

    [[nodiscard]] const std::optional<field_value_type>& value() const&& = delete;

    [[nodiscard]] const field_value_type& required_value() const&
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

    // Materializes an empty value when none is held. The empty json_value is
    // the token null, which a non-nullable field cannot hold (matching assign),
    // so ensure on such a field without a value throws and leaves it unchanged.
    [[nodiscard]] field_value_type& ensure(std::pmr::memory_resource* resource) {
        if (!value_) {
            if constexpr (detail::is_ruvia_json_value<field_value_type> && !nullable) {
                throw std::invalid_argument("ensuring an absent JSON value requires RUVIA_NULLABLE");
            } else {
                value_.emplace(
                    detail::make_request_value<field_value_type>(resolved_pmr_resource_tag{}, resource));
            }
        }
        state_ = detail::model_field_state::parsed;
        return *value_;
    }

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
        // A default never satisfies a required input field.
        if constexpr (!required) {
            if (state_ != detail::model_field_state::missing) {
                return;
            }
            options_type::apply_default([this, resource]<typename input_type>(input_type&& value) {
                this->assign(std::forward<input_type>(value), resource);
            });
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

    static constexpr auto quoted_wire_name_colon_storage_ =
        make_quoted_wire_name_colon<field_wire_name>();
    static constexpr auto comma_quoted_wire_name_colon_storage_ =
        make_comma_quoted_wire_name_colon<field_wire_name>();
    detail::model_field_state state_{detail::model_field_state::missing};
    bool present_{false};
    std::optional<field_value_type> value_;
};

}  // namespace ruvia::detail::model
