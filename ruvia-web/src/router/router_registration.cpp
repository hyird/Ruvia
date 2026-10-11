#include <algorithm>
#include <stdexcept>
#include <utility>

#include "ruvia/http/http_request_target.h"

#include "router/router_impl.h"

namespace ruvia {

using namespace detail;

namespace {

// The tightest ceiling any of the route's middlewares declared. Several may:
// a controller-wide one and a route-specific one, and the stricter must win
// rather than the last registered.
[[nodiscard]] std::size_t declared_request_body_limit(
    std::span<const detail::controller_middleware_descriptor> controller_middlewares,
    std::span<const detail::controller_middleware_descriptor> route_middlewares) noexcept {
    std::size_t limit = 0;
    const auto consider =
        [&limit](std::span<const detail::controller_middleware_descriptor> descriptors) noexcept {
            for (const auto& descriptor : descriptors) {
                const auto declared = descriptor.request_body_limit();
                if (declared != 0 && (limit == 0 || declared < limit)) {
                    limit = declared;
                }
            }
        };
    consider(controller_middlewares);
    consider(route_middlewares);
    return limit;
}

// The strictest deadline any of the route's middlewares declared, by the same
// rule the body limit uses: a narrower scope may only tighten.
[[nodiscard]] std::int64_t declared_deadline_ms(
    std::span<const detail::controller_middleware_descriptor> controller_middlewares,
    std::span<const detail::controller_middleware_descriptor> route_middlewares) noexcept {
    std::int64_t deadline_value = 0;
    const auto consider =
        [&deadline_value](std::span<const detail::controller_middleware_descriptor> descriptors) noexcept {
            for (const auto& descriptor : descriptors) {
                const auto declared = descriptor.deadline_ms();
                if (declared != 0 && (deadline_value == 0 || declared < deadline_value)) {
                    deadline_value = declared;
                }
            }
        };
    consider(controller_middlewares);
    consider(route_middlewares);
    return deadline_value;
}

}  // namespace

detail::route_entry::route_entry(std::pmr::memory_resource* resource, init_type init)
    : index_(init.index_),
      controller_(init.controller_),
      method_(init.method_),
      method_token_(init.method_token_, resource),
      path_(init.path_, resource),
      endpoint_(std::move(init.endpoint_)),
      dynamic_(init.dynamic_),
      max_request_body_bytes_(init.max_request_body_bytes_),
      deadline_ms_(init.deadline_ms_) {}

void detail::router_impl::register_endpoint(http_known_method method, std::string_view method_token,
    std::string_view path, route_endpoint endpoint, void* handler_target,
    std::span<const controller_middleware_descriptor> controller_middlewares,
    std::span<const controller_middleware_descriptor> route_middlewares) {
    if (controller_target_ == nullptr) {
        throw std::logic_error("routes are declared only while compiling the route plan");
    }
    // Every worker binds route handlers to its own instance of this slot's
    // controller, so a handler must belong to the declaring controller.
    if (handler_target != controller_target_) {
        throw std::logic_error("route handler must be bound to the controller declaring it");
    }
    if (!method_token.empty()) {
        if (!is_valid_http_method_token(method_token)) {
            throw std::invalid_argument("extension route method must be a valid HTTP method token");
        }
        // A method the framework classifies keeps the enum as its identity, so
        // the enum-indexed lookup stays the single authority for it. Accepting
        // both spellings would mean two routes for one method that only one of
        // the two lookups can ever find.
        if (classify_http_method(method_token) != http_known_method::unknown) {
            throw std::invalid_argument(
                "extension route method is a known method; register it with its typed route macro");
        }
    }
    validate_route_target(method, method_token, path, endpoint);

    auto& routes = plan_->routes_;
    const auto* tunnel = endpoint.tunnel();
    // Authority equality includes case and numeric-port normalization. Tunnel
    // candidates therefore share a protocol hash and use that equality below.
    auto hash = route_table::route_hash(
        method, route_table::path_hash(tunnel == nullptr ? std::string_view(path) : tunnel->protocol()));
    if (!method_token.empty()) {
        hash ^= route_table::path_hash(method_token);
    }
    const auto [first, last] = route_indices_.equal_range(hash);
    for (auto it = first; it != last; ++it) {
        const auto& registered = routes[it->second];
        if (registered.method() != method || registered.method_token() != method_token) {
            continue;
        }
        if (tunnel != nullptr) {
            const auto* registered_tunnel = registered.endpoint().tunnel();
            if (registered_tunnel != nullptr && registered_tunnel->protocol() == tunnel->protocol() &&
                (registered.path() == path ||
                    (tunnel->protocol().empty() && http_authorities_equal(path, registered.path(), 0)))) {
                throw std::invalid_argument("duplicate CONNECT route registration");
            }
        } else if (registered.path() == path) {
            throw std::invalid_argument("duplicate route registration");
        }
    }

    const bool dynamic = (tunnel == nullptr || !tunnel->protocol().empty()) && is_dynamic_path(path);
    const auto index = routes.size();
    // Any failure here aborts the whole plan compile, so nothing is rolled back.
    append_route_middlewares(controller_middlewares, route_middlewares);
    routes.emplace_back(plan_->resource_,
        route_entry::init_type{.index_ = index,
            .controller_ = controller_slot_,
            .method_ = method,
            .method_token_ = method_token,
            .path_ = path,
            .endpoint_ = std::move(endpoint),
            .dynamic_ = dynamic,
            .max_request_body_bytes_ = declared_request_body_limit(controller_middlewares, route_middlewares),
            .deadline_ms_ = declared_deadline_ms(controller_middlewares, route_middlewares)});
    route_indices_.emplace(hash, index);
}

}  // namespace ruvia
