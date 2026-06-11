#pragma once

#include <charconv>
#include <cstddef>
#include <filesystem>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <vector>

#include "ruvia/memory/MemoryPool.h"

namespace ruvia {

struct DotenvOptions {
    bool overrideExisting{false};
    bool required{false};
};

struct DotenvResult {
    bool loaded{false};
    std::size_t variablesSet{0};
    std::size_t variablesSkipped{0};
};

class Env final {
public:
    [[nodiscard]] std::optional<std::string_view> get(std::string_view name) const noexcept;

    template <typename T>
    [[nodiscard]] std::optional<std::remove_cvref_t<T>> get(std::string_view name) const noexcept;

    [[nodiscard]] bool loaded() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

private:
    friend class App;

    struct Variable {
        std::pmr::string name{ProcessMemory::instance().upstreamResource()};
        std::pmr::string value{ProcessMemory::instance().upstreamResource()};
    };

    DotenvResult loadFromExecutableDirectory(DotenvOptions options);
    DotenvResult loadFromFile(const std::filesystem::path& path, DotenvOptions options);
    std::pmr::vector<Variable>::const_iterator findVariable(std::string_view name) const noexcept;
    std::pmr::vector<Variable>::iterator findInsertPosition(std::string_view name) noexcept;
    [[nodiscard]] static std::optional<bool> parseBoolValue(std::string_view value) noexcept;

    template <typename T>
    [[nodiscard]] static std::optional<std::remove_cvref_t<T>> parseTypedValue(std::string_view value) noexcept;

    template <typename T>
    [[nodiscard]] static std::optional<T> parseArithmeticValue(std::string_view value) noexcept;

    template <typename>
    static constexpr bool kUnsupportedTypedEnvValue = false;

    std::pmr::vector<Variable> variables_{ProcessMemory::instance().upstreamResource()};
    bool loaded_{false};
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
