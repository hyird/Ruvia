#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>
#include <variant>

#include "ruvia/web/detail/router/RouteLimits.h"

// Lightweight, self-contained route-resolution result types. A materialized
// resolution is exactly one of resolved, method-not-allowed, or not-found; the
// payload for another alternative is not observable.

namespace ruvia::detail {

class RouteEntry;

class RouteMatch final {
public:
    [[nodiscard]] std::span<const std::string_view> values() const& noexcept {
        return std::span<const std::string_view>(paramValues_.data(), paramCount_);
    }
    [[nodiscard]] std::span<const std::string_view> values() const&& = delete;

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

class RouteNotFound final {
private:
    friend class RouteResolution;
    constexpr RouteNotFound() noexcept = default;
};

class RouteMethodNotAllowed final {
public:
    [[nodiscard]] constexpr std::uint32_t allowedMethods() const noexcept {
        return allowedMethods_;
    }

private:
    friend class RouteResolution;

    explicit constexpr RouteMethodNotAllowed(std::uint32_t allowedMethods) noexcept
        : allowedMethods_(allowedMethods) {}

    std::uint32_t allowedMethods_;
};

class ResolvedRoute final {
public:
    [[nodiscard]] const RouteEntry& route() const noexcept {
        return *route_;
    }

    // Static routes carry an empty match; dynamic routes carry their captured
    // values in the same value object. Callers never need a nullable match side
    // channel.
    [[nodiscard]] const RouteMatch& match() const& noexcept {
        return match_;
    }
    [[nodiscard]] const RouteMatch& match() const&& = delete;

private:
    friend class RouteResolution;

    ResolvedRoute(const RouteEntry& route, RouteMatch match) noexcept
        : route_(&route),
          match_(std::move(match)) {}

    const RouteEntry* route_;
    RouteMatch match_;
};

class RouteResolution final {
public:
    RouteResolution() noexcept
        : value_(RouteNotFound{}) {}

    [[nodiscard]] static RouteResolution resolved(const RouteEntry& route, RouteMatch match = {}) noexcept {
        return RouteResolution(ResolvedRoute(route, std::move(match)));
    }

    // An empty mask used to mean "no resource here", because a resource that
    // existed always had at least one representable method. Extension-method
    // routes break that: the resource exists but the mask -- which only spans
    // the classified methods -- cannot say so. `resourceExists` carries that
    // fact separately, so an extension-only path answers 405 with an Allow
    // built from its tokens instead of collapsing to 404.
    [[nodiscard]] static RouteResolution methodNotAllowed(std::uint32_t allowedMethods, bool resourceExists = false) noexcept {
        if (allowedMethods == 0 && !resourceExists) {
            return RouteResolution();
        }
        return RouteResolution(RouteMethodNotAllowed(allowedMethods));
    }

    [[nodiscard]] const ResolvedRoute* resolved() const& noexcept {
        return std::get_if<ResolvedRoute>(&value_);
    }
    [[nodiscard]] const ResolvedRoute* resolved() const&& = delete;

    [[nodiscard]] const RouteMethodNotAllowed* methodNotAllowed() const& noexcept {
        return std::get_if<RouteMethodNotAllowed>(&value_);
    }
    [[nodiscard]] const RouteMethodNotAllowed* methodNotAllowed() const&& = delete;

    [[nodiscard]] const RouteNotFound* notFound() const& noexcept {
        return std::get_if<RouteNotFound>(&value_);
    }
    [[nodiscard]] const RouteNotFound* notFound() const&& = delete;

private:
    using Value = std::variant<RouteNotFound, RouteMethodNotAllowed, ResolvedRoute>;

    template <typename Alternative>
    explicit RouteResolution(Alternative alternative) noexcept
        : value_(std::move(alternative)) {}

    Value value_;
};

}  // namespace ruvia::detail
