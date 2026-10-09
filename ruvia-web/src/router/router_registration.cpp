#include "ruvia/http/http_request_target.h"
#include "ruvia/http/http_response_stream.h"
#include "ruvia/web/detail/util/registration_resource.h"

#include "router/router_impl.h"

namespace ruvia {

using namespace detail;

detail::router_impl::pending_route_type::pending_route_type(std::pmr::memory_resource* resource, init_type init)
    : method_(init.method_),
      method_token_(init.method_token_, resource),
      path_(resource),
      endpoint_(std::move(init.endpoint_)),
      dynamic_(init.dynamic_),
      max_request_body_bytes_(init.max_request_body_bytes_),
      deadline_ms_(init.deadline_ms_),
      middlewares_(resource) {
    auto* const route_resource = path_.get_allocator().resource();
    if (init.path_.get_allocator().resource() == route_resource) {
        path_ = std::move(init.path_);
    } else {
        path_.assign(init.path_.data(), init.path_.size());
    }

    if (init.middlewares_.get_allocator().resource() == route_resource) {
        middlewares_ = std::move(init.middlewares_);
    } else {
        middlewares_.insert(middlewares_.end(), init.middlewares_.begin(), init.middlewares_.end());
    }
}

namespace {
[[nodiscard]] bool declared_replay_safe(
    std::span<const detail::controller_middleware_descriptor> controller_middlewares,
    std::span<const detail::controller_middleware_descriptor> route_middlewares) noexcept;
}

void detail::router_impl::register_route(http_known_method method, std::pmr::string path,
    route_handler_type handler, request_body_mode body_mode,
    std::span<const controller_middleware_descriptor> controller_middlewares,
    std::span<const controller_middleware_descriptor> route_middlewares) {
    register_endpoint(method, std::move(path),
        route_endpoint::buffered(handler, body_mode,
            declared_replay_safe(controller_middlewares, route_middlewares)),
        controller_middlewares, route_middlewares);
}

void detail::router_impl::register_response_stream_route(http_known_method method, std::pmr::string path,
    route_stream_handler_type handler,
    std::span<const controller_middleware_descriptor> controller_middlewares,
    std::span<const controller_middleware_descriptor> route_middlewares) {
    register_endpoint(method, std::move(path),
        route_endpoint::response_stream(handler, http_response_stream_kind::generic), controller_middlewares,
        route_middlewares);
}

void detail::router_impl::register_sse_route(http_known_method method, std::pmr::string path,
    route_stream_handler_type handler,
    std::span<const controller_middleware_descriptor> controller_middlewares,
    std::span<const controller_middleware_descriptor> route_middlewares) {
    register_endpoint(method, std::move(path),
        route_endpoint::response_stream(handler, http_response_stream_kind::sse), controller_middlewares,
        route_middlewares);
}

void detail::router_impl::register_tunnel_route(std::string_view protocol, std::pmr::string target,
    route_stream_handler_type handler, std::span<const controller_middleware_descriptor> controller_middlewares,
    std::span<const controller_middleware_descriptor> route_middlewares, http_tunnel_route_config config) {
    register_endpoint(http_known_method::connect, std::move(target), route_endpoint::tunnel(resource_, handler, protocol, config),
        controller_middlewares, route_middlewares);
}

void detail::router_impl::register_websocket_route(http_known_method method, std::pmr::string path,
    route_stream_handler_type handler,
    std::span<const controller_middleware_descriptor> controller_middlewares,
    std::span<const controller_middleware_descriptor> route_middlewares,
    websocket_route_config websocket_config) {
    register_endpoint(method, std::move(path),
        route_endpoint::get_websocket(resource_, handler, std::move(websocket_config)),
        controller_middlewares, route_middlewares);
}

namespace {

// The tightest ceiling any of the route's middlewares declared. Several may:
// a controller-wide one and a route-specific one, and the stricter must win
// rather than the last registered.
[[nodiscard]] bool declared_replay_safe(
    std::span<const detail::controller_middleware_descriptor> controller_middlewares,
    std::span<const detail::controller_middleware_descriptor> route_middlewares) noexcept {
    if (controller_middlewares.empty() && route_middlewares.empty()) {
        return false;
    }
    const auto all_declared = [](std::span<const detail::controller_middleware_descriptor> values) noexcept {
        return std::ranges::all_of(values, [](const auto& value) {
            return value.replay_safe();
        });
    };
    return all_declared(controller_middlewares) && all_declared(route_middlewares);
}

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

void detail::router_impl::register_endpoint(http_known_method method, std::pmr::string path,
    route_endpoint endpoint, std::span<const controller_middleware_descriptor> controller_middlewares,
    std::span<const controller_middleware_descriptor> route_middlewares) {
    register_endpoint_with_token(
        method, {}, std::move(path), std::move(endpoint), controller_middlewares, route_middlewares);
}

void detail::router_impl::register_endpoint_with_token(http_known_method method,
    std::string_view method_token, std::pmr::string path, route_endpoint endpoint,
    std::span<const controller_middleware_descriptor> controller_middlewares,
    std::span<const controller_middleware_descriptor> route_middlewares) {
    append_pending_route(pending_route_type(resource_,
        pending_route_type::init_type{.method_ = method,
            .method_token_ = std::pmr::string(method_token, resource_),
            .path_ = std::move(path),
            .endpoint_ = std::move(endpoint),
            .dynamic_ = false,
            .max_request_body_bytes_ =
                declared_request_body_limit(controller_middlewares, route_middlewares),
            .deadline_ms_ = declared_deadline_ms(controller_middlewares, route_middlewares),
            .middlewares_ = materialize_middlewares(controller_middlewares, route_middlewares)}));
}

void detail::router_impl::register_extension_method_route(std::string_view method_token,
    std::pmr::string path, route_handler_type handler, request_body_mode body_mode,
    std::span<const controller_middleware_descriptor> controller_middlewares,
    std::span<const controller_middleware_descriptor> route_middlewares) {
    if (!is_valid_http_method_token(method_token)) {
        throw std::invalid_argument("extension route method must be a valid HTTP method token");
    }
    // A method the framework classifies keeps the enum as its identity, so the
    // enum-indexed lookup stays the single authority for it. Accepting both
    // spellings would mean two routes for one method that only one of the two
    // lookups can ever find.
    if (classify_http_method(method_token) != http_known_method::unknown) {
        throw std::invalid_argument(
            "extension route method is a known method; register it with its typed route macro");
    }
    register_endpoint_with_token(http_known_method::unknown, method_token, std::move(path),
        route_endpoint::buffered(handler, body_mode,
            declared_replay_safe(controller_middlewares, route_middlewares)),
        controller_middlewares, route_middlewares);
}

void detail::router_impl::append_pending_route(pending_route_type route) {
    if (route_table_) {
        throw std::logic_error("cannot register route after router finalize");
    }
    validate_route_target(route.method(), route.method_token(), route.path(), route.endpoint());
    const auto* tunnel = route.endpoint().tunnel();
    // Authority equality includes case and numeric-port normalization. Tunnel
    // candidates therefore share a protocol hash and use that equality below.
    auto hash = route_table::route_hash(route.method(),
        route_table::path_hash(tunnel == nullptr ? route.path() : tunnel->protocol()));
    if (!route.method_token().empty()) {
        hash ^= route_table::path_hash(route.method_token());
    }
    const auto [first, last] = pending_route_indices_->equal_range(hash);
    for (auto it = first; it != last; ++it) {
        const auto& registered = pending_routes_[it->second];
        if (registered.method() != route.method() || registered.method_token() != route.method_token()) {
            continue;
        }
        if (tunnel != nullptr) {
            const auto* registered_tunnel = registered.endpoint().tunnel();
            if (registered_tunnel != nullptr && registered_tunnel->protocol() == tunnel->protocol() &&
                (route.path() == registered.path() ||
                    (tunnel->protocol().empty() && http_authorities_equal(route.path(), registered.path(), 0)))) {
                throw std::invalid_argument("duplicate CONNECT route registration");
            }
        } else if (registered.path() == route.path()) {
            throw std::invalid_argument("duplicate route registration");
        }
    }
    route.set_dynamic((tunnel == nullptr || !tunnel->protocol().empty()) && route_table::is_dynamic_path(route.path()));
    const auto index = pending_routes_.size();
    pending_routes_.push_back(std::move(route));
    try {
        pending_route_indices_->emplace(hash, index);
    } catch (...) {
        pending_routes_.pop_back();
        throw;
    }
}

}  // namespace ruvia
