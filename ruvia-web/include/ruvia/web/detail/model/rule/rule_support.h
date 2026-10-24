#pragma once

#include <cstddef>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

#include "ruvia/http/http_number_format.h"
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
    if (at == std::string_view::npos || at == 0 || at + 1 >= value.size() ||
        value.find('@', at + 1) != std::string_view::npos) {
        return false;
    }
    const auto domain = value.substr(at + 1);
    if (domain.front() == '.' || domain.back() == '.' ||
        domain.find('.') == std::string_view::npos ||
        domain.find("..") != std::string_view::npos) {
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
    using value_type = std::remove_cvref_t<t_type>;
    if constexpr (detail::is_ruvia_bytes<value_type>) {
        return "must be a padded base64 string";
    } else if constexpr (detail::is_ruvia_string<value_type>) {
        return "must be a string";
    } else if constexpr (detail::is_ruvia_array<value_type> || detail::is_ruvia_boxed_array<value_type>) {
        return "must be an array";
    } else if constexpr (detail::is_ruvia_json_object<value_type> || is_model<value_type>) {
        return "must be an object";
    } else if constexpr (detail::is_ruvia_json_value<value_type>) {
        return "must be a JSON value";
    } else if constexpr (detail::is_ruvia_scalar<value_type>) {
        if constexpr (std::is_same_v<detail::model_scalar_value_type<value_type>, bool>) {
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
    using value_type = std::remove_cvref_t<t_type>;
    return detail::is_ruvia_string<value_type> || detail::is_ruvia_bytes<value_type> ||
           detail::is_ruvia_array<value_type> || detail::is_ruvia_boxed_array<value_type>;
}

template <typename t_type>
[[nodiscard]] constexpr bool model_has_number_rule() noexcept {
    using value_type = std::remove_cvref_t<t_type>;
    if constexpr (detail::is_ruvia_scalar<value_type>) {
        using scalar_type = detail::model_scalar_value_type<value_type>;
        return std::is_arithmetic_v<scalar_type> && !std::is_same_v<scalar_type, bool>;
    } else {
        return false;
    }
}

// A validation path as segments on the traversal stack. Text is rendered only
// when an issue is emitted, so successful validation builds no path strings.
class model_path final {
public:
    constexpr model_path() noexcept = default;

    constexpr model_path(const model_path& parent, std::string_view field) noexcept
        : parent_(&parent),
          field_(field) {}

    constexpr model_path(const model_path& parent, std::size_t index) noexcept
        : parent_(&parent),
          index_(index),
          indexed_(true) {}

    model_path(const model_path&) = delete;
    model_path& operator=(const model_path&) = delete;

    void render(std::pmr::string& output) const {
        output.clear();
        append_to(output);
    }

private:
    void append_to(std::pmr::string& output) const {
        if (parent_ == nullptr) {
            return;
        }
        parent_->append_to(output);
        if (indexed_) {
            output.push_back('[');
            http_append_unsigned(output, index_);
            output.push_back(']');
            return;
        }
        if (!output.empty()) {
            output.push_back('.');
        }
        output.append(field_);
    }

    const model_path* parent_{nullptr};
    std::string_view field_{};
    std::size_t index_{0};
    bool indexed_{false};
};

// Forwards issues to a validator, rendering their paths into one scratch
// string that is created by the first issue and reused for later ones.
template <typename validator_type>
class model_issue_sink final {
public:
    explicit model_issue_sink(validator_type& validator_value) noexcept
        : validator_(validator_value) {}

    void add(const model_path& path, std::string_view code, std::string_view message) {
        if (!path_) {
            path_.emplace(validator_.resource());
        }
        path.render(*path_);
        validator_.add(*path_, code, message);
    }

private:
    validator_type& validator_;
    std::optional<std::pmr::string> path_;
};

template <typename rule_type>
[[nodiscard]] constexpr bool is_default_rule() noexcept {
    using unwrapped_rule_type = std::remove_cvref_t<rule_type>;
    return requires { typename unwrapped_rule_type::ruvia_default_rule_marker_type; };
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
    using unwrapped_rule_type = std::remove_cvref_t<rule_type>;
    return requires { typename unwrapped_rule_type::ruvia_initial_rule_marker_type; };
}

template <typename rule_type>
[[nodiscard]] constexpr bool is_model_option() noexcept {
    using unwrapped_rule_type = std::remove_cvref_t<rule_type>;
    return requires { typename unwrapped_rule_type::ruvia_model_option_marker_type; };
}

template <typename rule_type>
[[nodiscard]] constexpr bool is_validation_rule() noexcept {
    using unwrapped_rule_type = std::remove_cvref_t<rule_type>;
    return requires { typename unwrapped_rule_type::ruvia_validation_rule_marker_type; };
}

}  // namespace ruvia::detail::model
