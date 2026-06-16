#pragma once

#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace ruvia::detail {

[[nodiscard]] inline std::pmr::memory_resource* resolveRedisResource(
    std::pmr::memory_resource* resource) noexcept {
    return resource == nullptr ? std::pmr::get_default_resource() : resource;
}

inline void appendRedisNumber(std::pmr::string& output, std::uint64_t value) {
    std::array<char, 32> buffer{};
    auto [ptr, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (ec != std::errc{}) {
        throw std::logic_error("failed to format redis number");
    }
    output.append(buffer.data(), static_cast<std::size_t>(ptr - buffer.data()));
}

inline void appendRedisNumber(std::pmr::string& output, std::int64_t value) {
    std::array<char, 32> buffer{};
    auto [ptr, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (ec != std::errc{}) {
        throw std::logic_error("failed to format redis number");
    }
    output.append(buffer.data(), static_cast<std::size_t>(ptr - buffer.data()));
}

[[nodiscard]] inline std::pmr::string redisIntString(
    std::int64_t value,
    std::pmr::memory_resource* resource) {
    std::pmr::string output(resolveRedisResource(resource));
    appendRedisNumber(output, value);
    return output;
}

[[nodiscard]] inline std::pmr::string redisScoreString(
    double value,
    std::pmr::memory_resource* resource) {
    if (!std::isfinite(value)) {
        throw std::invalid_argument("redis sorted set score must be finite");
    }
    std::array<char, 64> buffer{};
    auto [ptr, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (ec != std::errc{}) {
        throw std::invalid_argument("redis sorted set score is invalid");
    }
    return std::pmr::string(
        buffer.data(),
        static_cast<std::size_t>(ptr - buffer.data()),
        resolveRedisResource(resource));
}

[[nodiscard]] inline std::pmr::vector<std::string_view> viewRedisArgs(
    const std::pmr::vector<std::pmr::string>& args,
    std::pmr::memory_resource* resource) {
    std::pmr::vector<std::string_view> views(resolveRedisResource(resource));
    views.reserve(args.size());
    for (const auto& arg : args) {
        views.emplace_back(arg.data(), arg.size());
    }
    return views;
}

}  // namespace ruvia::detail
