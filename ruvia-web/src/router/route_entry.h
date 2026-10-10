#pragma once

#include <cstddef>
#include <span>
#include <string_view>

#include "ruvia/http/http_known_method.h"

#include "router/route_endpoint.h"

// One registered route: its method and path, the endpoint it runs, its captured
// parameter names, and the slice of the table's middleware it inherits.

namespace ruvia::detail {

class route_entry final {
public:
    struct init_type final {
        http_known_method method_;
        // Non-empty only for an extension-method route, where it is the exact,
        // case-sensitive wire token (RFC 9110 9.1) and `method` is unknown.
        // Known methods keep the enum as their identity so the routing fast
        // path never compares strings.
        std::string_view method_token_{};
        std::string_view path_;
        route_endpoint endpoint_;
        bool dynamic_{false};
        // 0 = no route-declared ceiling; the server default applies.
        std::size_t max_request_body_bytes_{0};
        // 0 = no route-declared deadline; the app-wide one applies.
        std::int64_t deadline_ms_{0};
        std::size_t middleware_offset_{0};
        std::size_t middleware_count_{0};
    };

    route_entry(std::pmr::memory_resource* resource, init_type init);
    route_entry(detail::resolved_pmr_resource_tag, std::pmr::memory_resource* resource, init_type init);
    route_entry(const route_entry&) = delete;
    route_entry& operator=(const route_entry&) = delete;
    route_entry(route_entry&&) noexcept = default;
    route_entry& operator=(route_entry&&) = delete;

    [[nodiscard]] http_known_method method() const noexcept {
        return method_;
    }

    // Empty unless this is an extension-method route.
    [[nodiscard]] std::string_view method_token() const noexcept {
        return method_token_;
    }

    [[nodiscard]] std::string_view path() const noexcept {
        return path_;
    }

    [[nodiscard]] const route_endpoint& endpoint() const noexcept {
        return endpoint_;
    }

    [[nodiscard]] bool dynamic() const noexcept {
        return dynamic_;
    }

    // A ceiling declared by one of this route's middlewares, or 0 for none.
    // Read before the body is accepted, so it bounds what is buffered rather
    // than what a handler later sees.
    [[nodiscard]] std::size_t max_request_body_bytes() const noexcept {
        return max_request_body_bytes_;
    }

    // A handler deadline declared by one of this route's middlewares, or 0.
    [[nodiscard]] std::int64_t deadline_ms() const noexcept {
        return deadline_ms_;
    }

    [[nodiscard]] std::span<const std::string_view> param_names() const noexcept {
        return param_names_;
    }

    [[nodiscard]] std::size_t middleware_offset() const noexcept {
        return middleware_offset_;
    }

    [[nodiscard]] std::size_t middleware_count() const noexcept {
        return middleware_count_;
    }

    [[nodiscard]] bool has_middleware() const noexcept {
        return middleware_count_ != 0;
    }

    // True when at least one frame in this route's range is a path-scoped
    // middleware whose scope covers only part of the route's request paths;
    // dispatch then checks the request path before running such a frame.
    [[nodiscard]] bool has_conditional_middleware() const noexcept {
        return conditional_middleware_;
    }

    void set_middleware_range(std::size_t offset, std::size_t count, bool conditional) noexcept {
        middleware_offset_ = offset;
        middleware_count_ = count;
        conditional_middleware_ = conditional;
    }

    void set_param_names(std::span<const std::string_view> names) noexcept {
        param_names_ = names;
    }

private:
    http_known_method method_;
    std::pmr::string method_token_;
    std::pmr::string path_;
    route_endpoint endpoint_;
    bool dynamic_{false};
    std::size_t max_request_body_bytes_{0};
    std::int64_t deadline_ms_{0};
    std::span<const std::string_view> param_names_{};
    std::size_t middleware_offset_{0};
    std::size_t middleware_count_{0};
    bool conditional_middleware_{false};
};

}  // namespace ruvia::detail
