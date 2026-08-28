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

struct EnvAccess;
struct DotenvResultAccess;
struct EnvState;

}  // namespace detail

enum class DotenvExistingVariablePolicy : std::uint8_t {
    kPreserve,
    kOverride,
};

enum class DotenvMissingFilePolicy : std::uint8_t {
    kIgnore,
    kRequire,
};

struct DotenvOptions {
    DotenvExistingVariablePolicy existingVariables{DotenvExistingVariablePolicy::kPreserve};
    DotenvMissingFilePolicy missingFile{DotenvMissingFilePolicy::kIgnore};
};

class DotenvResult final {
public:
    [[nodiscard]] bool loaded() const noexcept {
        return loaded_;
    }

    [[nodiscard]] std::size_t variablesSet() const noexcept {
        return variablesSet_;
    }

    [[nodiscard]] std::size_t variablesSkipped() const noexcept {
        return variablesSkipped_;
    }

private:
    friend struct detail::DotenvResultAccess;

    explicit DotenvResult(bool loaded) noexcept
        : loaded_(loaded) {}

    bool loaded_{false};
    std::size_t variablesSet_{0};
    std::size_t variablesSkipped_{0};
};

class Env final {
public:
    Env();
    ~Env();

    Env(const Env&) = delete;
    Env& operator=(const Env&) = delete;
    Env(Env&&) = delete;
    Env& operator=(Env&&) = delete;

    [[nodiscard]] std::optional<std::string_view> get(std::string_view name) const& noexcept;
    [[nodiscard]] std::optional<std::string_view> get(std::string_view) const&& = delete;

    // Typed lookup distinguishes an absent variable from a malformed one:
    // absence returns nullopt, while a present value that cannot be parsed as
    // T throws std::invalid_argument. Falling back with value_or() is
    // therefore safe for optional deployment settings, but cannot hide a
    // misspelled port, limit, or boolean in the environment -- which is the
    // point: a typo in a deployment variable should stop startup, not silently
    // select the default.
    template <typename T>
    [[nodiscard]] std::optional<std::remove_cvref_t<T>> get(std::string_view name) const&;

    template <typename T>
    [[nodiscard]] std::optional<std::remove_cvref_t<T>> get(std::string_view) const&& = delete;

    // The same lookup for a caller that treats a malformed value as absent
    // rather than fatal -- an optional feature flag, say. Absent and malformed
    // are indistinguishable here by design; use get<T>() when the difference
    // matters, which is the common case for deployment settings.
    template <typename T>
    [[nodiscard]] std::optional<std::remove_cvref_t<T>> tryGet(std::string_view name) const& noexcept {
        const auto value = get(name);
        if (!value.has_value()) {
            return std::nullopt;
        }
        return parseTypedValue<T>(*value);
    }

    template <typename T>
    [[nodiscard]] std::optional<std::remove_cvref_t<T>> tryGet(std::string_view) const&& = delete;

    [[nodiscard]] bool loaded() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

private:
    struct StateDeleter final {
        void operator()(detail::EnvState* state) const noexcept;
    };

    friend struct detail::EnvAccess;

    [[nodiscard]] static std::optional<bool> parseBoolValue(std::string_view value) noexcept;

    template <typename T>
    [[nodiscard]] static std::optional<std::remove_cvref_t<T>> parseTypedValue(std::string_view value) noexcept;

    template <typename T>
    [[nodiscard]] static std::optional<T> parseArithmeticValue(std::string_view value) noexcept;

    template <typename>
    static constexpr bool kUnsupportedTypedEnvValue = false;

    std::unique_ptr<detail::EnvState, StateDeleter> state_;
};

template <typename T>
std::optional<std::remove_cvref_t<T>> Env::get(std::string_view name) const& {
    const auto value = get(name);
    if (!value.has_value()) {
        return std::nullopt;
    }
    auto parsed = parseTypedValue<T>(*value);
    if (!parsed.has_value()) {
        throw std::invalid_argument("invalid value for environment variable '" + std::string(name) + "'");
    }
    return parsed;
}

template <typename T>
std::optional<std::remove_cvref_t<T>> Env::parseTypedValue(std::string_view value) noexcept {
    using Value = std::remove_cvref_t<T>;

    if constexpr (std::is_same_v<Value, std::string_view>) {
        return value;
    } else if constexpr (std::is_same_v<Value, bool>) {
        return parseBoolValue(value);
    } else if constexpr (std::is_integral_v<Value> || std::is_floating_point_v<Value>) {
        return parseArithmeticValue<Value>(value);
    } else {
        static_assert(kUnsupportedTypedEnvValue<Value>, "Env::get<T>() supports string_view, bool, integral, and floating-point values");
    }
}

template <typename T>
std::optional<T> Env::parseArithmeticValue(std::string_view value) noexcept {
    if (value.empty()) {
        return std::nullopt;
    }

    T parsed{};
    const auto* first = value.data();
    const auto* last = value.data() + value.size();
    const auto [ptr, ec] = std::from_chars(first, last, parsed);
    if (ec != std::errc{} || ptr != last) {
        return std::nullopt;
    }
    if constexpr (std::is_floating_point_v<T>) {
        if (!std::isfinite(parsed)) {
            return std::nullopt;
        }
    }

    return parsed;
}

}  // namespace ruvia
