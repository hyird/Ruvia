#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>
#include <variant>

#include "router/route_limits.h"

// Lightweight, self-contained route-resolution result types. A materialized
// resolution is exactly one of resolved, method-not-allowed, or not-found; the
// payload for another alternative is not observable.

namespace ruvia::detail {

class route_entry;

class route_match final {
public:
    [[nodiscard]] std::span<const std::string_view> values() const& noexcept {
        return std::span<const std::string_view>(param_values_.data(), param_count_);
    }
    [[nodiscard]] std::span<const std::string_view> values() const&& = delete;

    [[nodiscard]] std::size_t size() const noexcept {
        return param_count_;
    }

    void clear() noexcept {
        param_count_ = 0;
    }

    void truncate(std::size_t count) noexcept {
        param_count_ = count <= param_count_ ? count : param_count_;
    }

    [[nodiscard]] bool add(std::string_view value) noexcept {
        if (param_count_ >= param_values_.size()) {
            return false;
        }

        param_values_[param_count_++] = value;
        return true;
    }

private:
    std::array<std::string_view, max_route_params> param_values_{};
    std::size_t param_count_{0};
};

class route_not_found final {
private:
    friend class route_resolution;
    constexpr route_not_found() noexcept = default;
};

class route_method_not_allowed final {
public:
    [[nodiscard]] constexpr std::uint32_t allowed_methods() const noexcept {
        return allowed_methods_;
    }

private:
    friend class route_resolution;

    explicit constexpr route_method_not_allowed(std::uint32_t allowed_methods) noexcept
        : allowed_methods_(allowed_methods) {}

    std::uint32_t allowed_methods_;
};

class resolved_route final {
public:
    [[nodiscard]] const route_entry& route() const noexcept {
        return *route_;
    }

    // Static routes carry an empty match; dynamic routes carry their captured
    // values in the same value object. Callers never need a nullable match side
    // channel.
    [[nodiscard]] const route_match& match() const& noexcept {
        return match_;
    }
    [[nodiscard]] const route_match& match() const&& = delete;

private:
    friend class route_resolution;

    resolved_route(const route_entry& route, route_match match) noexcept
        : route_(&route),
          match_(match) {}

    const route_entry* route_;
    route_match match_;
};

class route_resolution final {
public:
    route_resolution() noexcept
        : value_(route_not_found{}) {}

    [[nodiscard]] static route_resolution resolved(
        const route_entry& route, route_match match = {}) noexcept {
        return route_resolution(resolved_route(route, match));
    }

    // The mask spans only classified methods, so it can be empty for a resource
    // that has extension-method routes. `resource_exists` carries that fact
    // separately, ensuring an extension-only path answers 405 with an Allow
    // built from its tokens instead of collapsing to 404.
    [[nodiscard]] static route_resolution method_not_allowed(
        std::uint32_t allowed_methods, bool resource_exists = false) noexcept {
        if (allowed_methods == 0 && !resource_exists) {
            return route_resolution();
        }
        return route_resolution(route_method_not_allowed(allowed_methods));
    }

    [[nodiscard]] const resolved_route* resolved() const& noexcept {
        return std::get_if<resolved_route>(&value_);
    }
    [[nodiscard]] const resolved_route* resolved() const&& = delete;

    [[nodiscard]] const route_method_not_allowed* method_not_allowed() const& noexcept {
        return std::get_if<route_method_not_allowed>(&value_);
    }
    [[nodiscard]] const route_method_not_allowed* method_not_allowed() const&& = delete;

    [[nodiscard]] const route_not_found* not_found() const& noexcept {
        return std::get_if<route_not_found>(&value_);
    }
    [[nodiscard]] const route_not_found* not_found() const&& = delete;

private:
    using value_type = std::variant<route_not_found, route_method_not_allowed, resolved_route>;

    template <typename alternative_type>
    explicit route_resolution(alternative_type alternative) noexcept
        : value_(std::move(alternative)) {}

    value_type value_;
};

}  // namespace ruvia::detail
