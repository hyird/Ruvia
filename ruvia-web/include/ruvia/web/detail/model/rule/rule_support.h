#pragma once

#include <charconv>
#include <cstddef>
#include <memory_resource>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>

#include "ruvia/web/detail/model/rule/rule_types.h"
#include "ruvia/web/detail/model/traits.h"

namespace ruvia::detail::model {

template <typename t_type>
[[nodiscard]] std::size_t model_size(const t_type& value) noexcept {
    if constexpr (requires { value.view(); }) {
        return value.view().size();
    } else {
        return value.size();
    }
}

template <typename t_type>
[[nodiscard]] std::string_view model_string(const t_type& value) noexcept {
    if constexpr (requires { value.view(); }) {
        return value.view();
    } else {
        return std::string_view(value);
    }
}

[[nodiscard]] inline bool is_email_like(std::string_view value) noexcept {
    const auto at = value.find('@');
    if (at == std::string_view::npos || at == 0 || at + 1 >= value.size()) {
        return false;
    }
    const auto dot = value.find('.', at + 1);
    if (dot == std::string_view::npos || dot + 1 >= value.size()) {
        return false;
    }
    for (const char c : value) {
        // Compare as unsigned: `char` is signed on most targets, so a UTF-8 byte
        // (>= 0x80) is negative and would satisfy `c <= 0x20`, wrongly rejecting an
        // internationalized address (RFC 6531) as if it held a control byte. The
        // guard only means to reject controls, SP, and DEL -- match the codebase's
        // other byte checks (e.g. is_valid_cookie_value) by using an unsigned byte.
        const auto byte = static_cast<unsigned char>(c);
        if (byte <= 0x20 || byte == 0x7F) {
            return false;
        }
    }
    return true;
}

template <typename t_type>
[[nodiscard]] bool is_empty_value(const t_type& value) noexcept {
    if constexpr (detail::is_ruvia_string<t_type> || detail::is_ruvia_bytes<t_type> ||
                  detail::is_ruvia_array<t_type> || detail::is_ruvia_boxed_array<t_type>) {
        return value.empty();
    } else {
        return false;
    }
}

template <typename t_type>
[[nodiscard]] constexpr std::string_view expected_type_name() noexcept {
    using value_t_type = std::remove_cvref_t<t_type>;
    if constexpr (detail::is_ruvia_bytes<value_t_type>) {
        return "must be a padded base64 string";
    } else if constexpr (detail::is_ruvia_string<value_t_type>) {
        return "must be a string";
    } else if constexpr (detail::is_ruvia_array<value_t_type> || detail::is_ruvia_boxed_array<value_t_type>) {
        return "must be an array";
    } else if constexpr (detail::is_ruvia_json_object<value_t_type> || is_model<value_t_type>) {
        return "must be an object";
    } else if constexpr (detail::is_ruvia_json_value<value_t_type>) {
        return "must be a JSON value";
    } else if constexpr (detail::is_ruvia_scalar<value_t_type>) {
        if constexpr (std::is_same_v<detail::model_scalar_value_t_type<value_t_type>, bool>) {
            return "must be a boolean";
        } else {
            return "must be a number";
        }
    } else {
        return "has invalid type";
    }
}

template <typename t_type>
[[nodiscard]] constexpr bool model_has_size_rule() noexcept {
    using value_t_type = std::remove_cvref_t<t_type>;
    return detail::is_ruvia_string<value_t_type> || detail::is_ruvia_bytes<value_t_type> ||
           detail::is_ruvia_array<value_t_type> || detail::is_ruvia_boxed_array<value_t_type>;
}

template <typename t_type>
[[nodiscard]] constexpr bool model_has_number_rule() noexcept {
    using value_t_type = std::remove_cvref_t<t_type>;
    if constexpr (detail::is_ruvia_scalar<value_t_type>) {
        using scalar_t_type = detail::model_scalar_value_t_type<value_t_type>;
        return std::is_arithmetic_v<scalar_t_type> && !std::is_same_v<scalar_t_type, bool>;
    } else {
        return false;
    }
}

inline void append_path(std::pmr::string& output, std::string_view prefix, std::string_view field) {
    output.clear();
    output.reserve(prefix.size() + (prefix.empty() ? 0 : 1) + field.size());
    if (!prefix.empty()) {
        output.append(prefix.data(), prefix.size());
        output.push_back('.');
    }
    output.append(field.data(), field.size());
}

inline void append_index_path(std::pmr::string& output, std::string_view prefix, std::size_t index) {
    output.clear();
    output.reserve(prefix.size() + 2 + 20);
    output.append(prefix.data(), prefix.size());
    output.push_back('[');
    char buffer[32];
    const auto [ptr, ec] = std::to_chars(buffer, buffer + sizeof(buffer), index);
    if (ec == std::errc{}) {
        output.append(buffer, static_cast<std::size_t>(ptr - buffer));
    }
    output.push_back(']');
}

template <typename rule_type>
[[nodiscard]] constexpr bool is_default_rule() noexcept {
    using rule_t_type = std::remove_cvref_t<rule_type>;
    return requires { typename rule_t_type::ruvia_default_rule_marker_type; };
}

template <auto provider>
struct initial final {
    using ruvia_initial_rule_marker_type = void;
    using ruvia_model_option_marker_type = void;

    [[nodiscard]] static constexpr decltype(auto) value() {
        return provider();
    }
};

template <typename rule_type>
[[nodiscard]] constexpr bool is_initial_rule() noexcept {
    using rule_t_type = std::remove_cvref_t<rule_type>;
    return requires { typename rule_t_type::ruvia_initial_rule_marker_type; };
}

template <typename rule_type>
[[nodiscard]] constexpr bool is_model_option() noexcept {
    using rule_t_type = std::remove_cvref_t<rule_type>;
    return requires { typename rule_t_type::ruvia_model_option_marker_type; };
}

template <typename rule_type>
[[nodiscard]] constexpr bool is_validation_rule() noexcept {
    using rule_t_type = std::remove_cvref_t<rule_type>;
    return requires { typename rule_t_type::ruvia_validation_rule_marker_type; };
}

}  // namespace ruvia::detail::model
