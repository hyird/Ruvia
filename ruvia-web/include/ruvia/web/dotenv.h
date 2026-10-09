#pragma once

#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>

namespace ruvia {

namespace detail {

struct env_access;
struct dotenv_result_access;
struct env_state;

}  // namespace detail

enum class dotenv_existing_variable_policy : std::uint8_t {
    preserve,
    override,
};

enum class dotenv_missing_file_policy : std::uint8_t {
    ignore,
    require,
};

struct dotenv_options {
    dotenv_existing_variable_policy existing_variables_{dotenv_existing_variable_policy::preserve};
    dotenv_missing_file_policy missing_file_{dotenv_missing_file_policy::ignore};
};

class dotenv_result final {
public:
    [[nodiscard]] bool loaded() const noexcept {
        return loaded_;
    }

    [[nodiscard]] std::size_t variables_set() const noexcept {
        return variables_set_;
    }

    [[nodiscard]] std::size_t variables_skipped() const noexcept {
        return variables_skipped_;
    }

private:
    friend struct detail::dotenv_result_access;

    explicit dotenv_result(bool loaded) noexcept
        : loaded_(loaded) {}

    bool loaded_{false};
    std::size_t variables_set_{0};
    std::size_t variables_skipped_{0};
};

class env final {
public:
    env();
    ~env();

    env(const env&) = delete;
    env& operator=(const env&) = delete;
    env(env&&) = delete;
    env& operator=(env&&) = delete;

    [[nodiscard]] std::optional<std::string_view> get(std::string_view name) const& noexcept;
    [[nodiscard]] std::optional<std::string_view> get(std::string_view) const&& = delete;

    // Typed lookup distinguishes an absent variable from a malformed one:
    // absence returns nullopt, while a present value that cannot be parsed as
    // T throws std::invalid_argument. Falling back with value_or() is
    // therefore safe for optional deployment settings, but cannot hide a
    // misspelled port, limit, or boolean in the environment -- which is the
    // point: a typo in a deployment variable should stop startup, not silently
    // select the default.
    template <typename t_type>
    [[nodiscard]] std::optional<std::remove_cvref_t<t_type>> get(std::string_view name) const&;

    template <typename t_type>
    [[nodiscard]] std::optional<std::remove_cvref_t<t_type>> get(std::string_view) const&& = delete;

    // The same lookup for a caller that treats a malformed value as absent
    // rather than fatal -- an optional feature flag, say. Absent and malformed
    // are indistinguishable here by design; use get<T>() when the difference
    // matters, which is the common case for deployment settings.
    template <typename t_type>
    [[nodiscard]] std::optional<std::remove_cvref_t<t_type>> try_get(
        std::string_view name) const& noexcept {
        const auto value = get(name);
        if (!value.has_value()) {
            return std::nullopt;
        }
        return parse_typed_value<t_type>(*value);
    }

    template <typename t_type>
    [[nodiscard]] std::optional<std::remove_cvref_t<t_type>> try_get(std::string_view) const&& = delete;

    [[nodiscard]] bool loaded() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

private:
    struct state_deleter_type {
        void operator()(detail::env_state* state) const noexcept;
    };

    friend struct detail::env_access;

    [[nodiscard]] static std::optional<bool> parse_bool_value(std::string_view value) noexcept;

    template <typename t_type>
    [[nodiscard]] static std::optional<std::remove_cvref_t<t_type>> parse_typed_value(
        std::string_view value) noexcept;

    template <typename t_type>
    [[nodiscard]] static std::optional<t_type> parse_arithmetic_value(std::string_view value) noexcept;

    template <typename>
    static constexpr bool unsupported_typed_env_value = false;

    std::unique_ptr<detail::env_state, state_deleter_type> state_;
};

template <typename t_type>
std::optional<std::remove_cvref_t<t_type>> env::get(std::string_view name) const& {
    const auto value = get(name);
    if (!value.has_value()) {
        return std::nullopt;
    }
    auto parsed_value = parse_typed_value<t_type>(*value);
    if (!parsed_value.has_value()) {
        throw std::invalid_argument(
            "invalid value for environment variable '" + std::string(name) + "'");
    }
    return parsed_value;
}

template <typename t_type>
std::optional<std::remove_cvref_t<t_type>> env::parse_typed_value(std::string_view value) noexcept {
    using value_type = std::remove_cvref_t<t_type>;

    if constexpr (std::is_same_v<value_type, std::string_view>) {
        return value;
    } else if constexpr (std::is_same_v<value_type, bool>) {
        return parse_bool_value(value);
    } else if constexpr (std::is_integral_v<value_type> || std::is_floating_point_v<value_type>) {
        return parse_arithmetic_value<value_type>(value);
    } else {
        static_assert(unsupported_typed_env_value<value_type>,
            "Env::get<T>() supports string_view, bool, integral, and floating-point values");
    }
}

template <typename t_type>
std::optional<t_type> env::parse_arithmetic_value(std::string_view value) noexcept {
    if (value.empty()) {
        return std::nullopt;
    }

    t_type parsed_value{};
    const auto* first = value.data();
    const auto* last = value.data() + value.size();
    const auto [ptr, ec] = std::from_chars(first, last, parsed_value);
    if (ec != std::errc{} || ptr != last) {
        return std::nullopt;
    }
    if constexpr (std::is_floating_point_v<t_type>) {
        if (!std::isfinite(parsed_value)) {
            return std::nullopt;
        }
    }

    return parsed_value;
}

}  // namespace ruvia
