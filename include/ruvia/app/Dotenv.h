#pragma once

#include <charconv>
#include <cstddef>
#include <memory>
#include <optional>
#include <string_view>
#include <system_error>
#include <type_traits>

namespace ruvia {

namespace detail {

struct EnvAccess;
struct DotenvResultAccess;
struct EnvState;
struct EnvStateDeleter final {
    void operator()(EnvState* state) const noexcept;
};

}  // namespace detail

struct DotenvOptions {
    bool overrideExisting{false};
    bool required{false};
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

namespace detail {

struct DotenvResultAccess final {
    [[nodiscard]] static DotenvResult make(bool loaded) noexcept {
        return DotenvResult(loaded);
    }

    static void incrementVariablesSet(DotenvResult& result) noexcept {
        ++result.variablesSet_;
    }

    static void incrementVariablesSkipped(DotenvResult& result) noexcept {
        ++result.variablesSkipped_;
    }
};

}  // namespace detail

class Env final {
public:
    Env();
    ~Env();

    Env(const Env&) = delete;
    Env& operator=(const Env&) = delete;
    Env(Env&&) = delete;
    Env& operator=(Env&&) = delete;

    [[nodiscard]] std::optional<std::string_view> get(std::string_view name) const noexcept;

    template <typename T>
    [[nodiscard]] std::optional<std::remove_cvref_t<T>> get(std::string_view name) const noexcept;

    [[nodiscard]] bool loaded() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

private:
    friend struct detail::EnvAccess;

    [[nodiscard]] static std::optional<bool> parseBoolValue(std::string_view value) noexcept;

    template <typename T>
    [[nodiscard]] static std::optional<std::remove_cvref_t<T>> parseTypedValue(std::string_view value) noexcept;

    template <typename T>
    [[nodiscard]] static std::optional<T> parseArithmeticValue(std::string_view value) noexcept;

    template <typename>
    static constexpr bool kUnsupportedTypedEnvValue = false;

    std::unique_ptr<detail::EnvState, detail::EnvStateDeleter> state_;
};

template <typename T>
std::optional<std::remove_cvref_t<T>> Env::get(std::string_view name) const noexcept {
    const auto value = get(name);
    if (!value) {
        return std::nullopt;
    }

    return parseTypedValue<T>(*value);
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

    return parsed;
}

}  // namespace ruvia
