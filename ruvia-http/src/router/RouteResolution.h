#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "ruvia/http/HttpTypes.h"

// Lightweight route-resolution result types. Kept below the RouteTable layer so
// the request hot path, including the connection work set that embeds
// RouteResolution, can name them without pulling in route indexes or router
// builder state. RouteEntry is only referenced through a pointer here, so a
// forward declaration suffices.

namespace ruvia::detail {

class RouteEntry;

class RouteMatch final {
public:
    [[nodiscard]] std::span<const std::string_view> values() const noexcept {
        return std::span<const std::string_view>(paramValues_.data(), paramCount_);
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return paramCount_;
    }

    void clear() noexcept {
        paramCount_ = 0;
    }

    void truncate(std::size_t count) noexcept {
        paramCount_ = count <= paramCount_ ? count : paramCount_;
    }

    [[nodiscard]] bool add(std::string_view value) noexcept {
        if (paramCount_ >= paramValues_.size()) {
            return false;
        }

        paramValues_[paramCount_++] = value;
        return true;
    }

private:
    std::array<std::string_view, kMaxRouteParams> paramValues_{};
    std::size_t paramCount_{0};
};

struct RouteResolution final {
    [[nodiscard]] static RouteResolution foundStatic(const RouteEntry* route) noexcept {
        RouteResolution resolution;
        resolution.route_ = route;
        return resolution;
    }

    [[nodiscard]] static RouteResolution foundDynamic(
        const RouteEntry* route,
        const RouteMatch& match) noexcept {
        RouteResolution resolution;
        resolution.route_ = route;
        resolution.match_ = &match;
        return resolution;
    }

    [[nodiscard]] static RouteResolution methodNotAllowed(std::uint32_t allowedMethods) noexcept {
        RouteResolution resolution;
        resolution.allowedMethods_ = allowedMethods;
        return resolution;
    }

    [[nodiscard]] bool found() const noexcept {
        return route_ != nullptr;
    }

    [[nodiscard]] bool methodNotAllowed() const noexcept {
        return route_ == nullptr && allowedMethods_ != 0;
    }

    [[nodiscard]] const RouteEntry& route() const noexcept {
        return *route_;
    }

    [[nodiscard]] const RouteMatch* match() const noexcept {
        return match_;
    }

    [[nodiscard]] std::uint32_t allowedMethods() const noexcept {
        return allowedMethods_;
    }

private:
    const RouteEntry* route_{nullptr};
    const RouteMatch* match_{nullptr};
    std::uint32_t allowedMethods_{0};
};

}  // namespace ruvia::detail
