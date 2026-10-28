#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>
#include <variant>

#include "ruvia/web/detail/router/route_handlers.h"

#include "router/route_limits.h"

// Lightweight, self-contained route-resolution result types. A materialized
// resolution is exactly one of resolved, method-not-allowed, not-found, or
// rejected; the payload for another alternative is not observable.

namespace ruvia::detail {

class route_entry;

class route_match final {
public:
    constexpr route_match() noexcept = default;

    constexpr route_match(const route_match& other) noexcept
        : param_count_(other.param_count_) {
        for (std::size_t i = 0; i < param_count_; ++i) {
            param_values_[i] = other.param_values_[i];
        }
    }

    constexpr route_match& operator=(const route_match& other) noexcept {
        param_count_ = other.param_count_;
        for (std::size_t i = 0; i < param_count_; ++i) {
            param_values_[i] = other.param_values_[i];
        }
        return *this;
    }

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

// A target that names no resource at all (rootless or dot-segment path): no
// route and no document-root file may answer it, only the not-found path.
class route_rejected final {
private:
    friend class route_resolution;
    constexpr route_rejected() noexcept = default;
};

class route_method_not_allowed final {
public:
    [[nodiscard]] constexpr std::uint32_t allowed_methods() const noexcept {
        return allowed_methods_;
    }
    // The distinct extension tokens registered on the resource, for Allow.
    // Borrowed from the compiled route plan.
    [[nodiscard]] constexpr std::span<const std::string_view> extension_methods() const noexcept {
        return extension_methods_;
    }

private:
    friend class route_resolution;

    constexpr route_method_not_allowed(
        std::uint32_t allowed_methods, std::span<const std::string_view> extension_methods) noexcept
        : allowed_methods_(allowed_methods),
          extension_methods_(extension_methods) {}

    std::uint32_t allowed_methods_;
    std::span<const std::string_view> extension_methods_;
};

class resolved_route final {
public:
    // Process-invariant route data from the compiled plan.
    [[nodiscard]] const route_entry& route() const noexcept {
        return *route_;
    }

    // The resolving worker's handler. handler() requires a buffered-response
    // endpoint; stream_handler() a response-stream, websocket or tunnel one.
    [[nodiscard]] route_handler handler() const noexcept;
    [[nodiscard]] route_stream_handler stream_handler() const noexcept;

    // Static routes carry an empty match; dynamic routes carry their captured
    // values in the same value object. Callers never need a nullable match side
    // channel.
    [[nodiscard]] const route_match& match() const& noexcept {
        return match_;
    }
    [[nodiscard]] const route_match& match() const&& = delete;

private:
    friend class route_resolution;

    resolved_route(const route_entry& route, void* handler_target) noexcept
        : route_(&route),
          handler_target_(handler_target) {}

    resolved_route(const route_entry& route, void* handler_target, const route_match& match) noexcept
        : route_(&route),
          handler_target_(handler_target),
          match_(match) {}

    const route_entry* route_;
    void* handler_target_;
    route_match match_;
};

class route_resolution final {
public:
    route_resolution() noexcept
        : value_(route_not_found{}) {}

    [[nodiscard]] static route_resolution resolved(
        const route_entry& route, void* handler_target) noexcept {
        return route_resolution(resolved_route(route, handler_target));
    }

    [[nodiscard]] static route_resolution resolved(
        const route_entry& route, void* handler_target, const route_match& match) noexcept {
        return route_resolution(resolved_route(route, handler_target, match));
    }

    // The mask spans only classified methods, so it can be empty for a resource
    // that has extension-method routes. A non-empty `extension_methods` alone
    // still makes the resource exist, so an extension-only path answers 405
    // with an Allow built from its tokens instead of collapsing to 404.
    [[nodiscard]] static route_resolution method_not_allowed(
        std::uint32_t allowed_methods, std::span<const std::string_view> extension_methods) noexcept {
        if (allowed_methods == 0 && extension_methods.empty()) {
            return route_resolution();
        }
        return route_resolution(route_method_not_allowed(allowed_methods, extension_methods));
    }

    [[nodiscard]] static route_resolution reject() noexcept {
        return route_resolution(route_rejected{});
    }

    [[nodiscard]] const resolved_route* resolved() const& noexcept {
        return std::get_if<resolved_route>(&value_);
    }
    [[nodiscard]] const resolved_route* resolved() const&& = delete;

    [[nodiscard]] const route_method_not_allowed* method_not_allowed() const& noexcept {
        return std::get_if<route_method_not_allowed>(&value_);
    }
    [[nodiscard]] const route_method_not_allowed* method_not_allowed() const&& = delete;

    [[nodiscard]] const route_rejected* rejected() const& noexcept {
        return std::get_if<route_rejected>(&value_);
    }
    [[nodiscard]] const route_rejected* rejected() const&& = delete;

private:
    using value_type = std::variant<route_not_found, route_method_not_allowed, resolved_route, route_rejected>;

    template <typename alternative_type>
    explicit route_resolution(alternative_type alternative) noexcept
        : value_(std::move(alternative)) {}

    value_type value_;
};

}  // namespace ruvia::detail
