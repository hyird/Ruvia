#include "ruvia/web/detail/util/registration_resource.h"
#include "ruvia/web/streaming.h"

#include "router/router_impl.h"

// The route-registration surface a controller sees. Each call builds the
// endpoint its route kind runs and hands it, with the scope's prefix and
// middlewares, to the router's single registration path.

namespace ruvia {

using namespace detail;

namespace {

std::pmr::string join_controller_paths(std::string_view prefix, std::string_view path) {
    auto* resource = registration_resource();
    if (prefix.empty() || prefix == "/") {
        if (path.empty()) {
            return std::pmr::string{"/", resource};
        }
        if (path.front() == '/') {
            return std::pmr::string(path, resource);
        }

        std::pmr::string output(resource);
        output.reserve(path.size() + 1);
        output.push_back('/');
        output.append(path);
        return output;
    }

    std::pmr::string output(resource);
    output.reserve(prefix.size() + path.size() + 1);
    output.append(prefix.front() == '/' ? prefix : std::string_view{});
    if (prefix.front() != '/') {
        output.push_back('/');
        output.append(prefix);
    }
    if (output.size() > 1 && output.back() == '/') {
        output.pop_back();
    }
    if (path.empty() || path == "/") {
        return output;
    }
    if (path.front() == '/') {
        path.remove_prefix(1);
    }
    output.push_back('/');
    output.append(path);
    return output;
}

[[nodiscard]] std::pmr::vector<controller_middleware_descriptor> merge_middleware_descriptors(
    std::span<const controller_middleware_descriptor> base, std::span<const controller_middleware_descriptor> extra) {
    std::pmr::vector<controller_middleware_descriptor> middlewares(registration_resource());
    middlewares.reserve(base.size() + extra.size());
    middlewares.insert(middlewares.end(), base.begin(), base.end());
    middlewares.insert(middlewares.end(), extra.begin(), extra.end());
    return middlewares;
}

[[nodiscard]] request_body_mode declared_request_body_mode(
    std::span<const controller_middleware_descriptor> controller_middlewares,
    std::span<const controller_middleware_descriptor> route_middlewares) noexcept {
    for (const auto descriptors : {controller_middlewares, route_middlewares}) {
        for (const auto& descriptor : descriptors) {
            if (descriptor.stream_request_body()) {
                return request_body_mode::stream;
            }
        }
    }
    return request_body_mode::buffered;
}
}  // namespace

detail::controller_route_builder::controller_route_builder(router& router_value, std::string_view prefix,
    std::pmr::vector<controller_middleware_descriptor> middlewares)
    : controller_route_builder(
          router_value, join_controller_paths({}, prefix), std::move(middlewares), owned_prefix_tag_type{}) {}

detail::controller_route_builder::controller_route_builder(router& router_value, std::pmr::string prefix,
    std::pmr::vector<controller_middleware_descriptor> middlewares, owned_prefix_tag_type)
    : router_(router_value),
      prefix_(std::move(prefix)),
      middlewares_(std::move(middlewares)) {}

detail::controller_route_builder::~controller_route_builder() = default;

void detail::controller_route_builder::register_route(http_known_method method, std::string_view path,
    route_handler handler, std::span<const controller_middleware_descriptor> middlewares) const {
    const auto body_mode = declared_request_body_mode(middlewares_, middlewares);
    router_.register_endpoint(method, {}, join_controller_paths(prefix_, path),
        route_endpoint::buffered(callback_access::invoke(handler), body_mode), callback_access::target(handler),
        middlewares_, middlewares);
}

void detail::controller_route_builder::register_route(http_known_method method, std::string_view path,
    route_stream_handler handler,
    std::span<const controller_middleware_descriptor> middlewares) const {
    const auto body_mode = declared_request_body_mode(middlewares_, middlewares);
    router_.register_endpoint(method, {}, join_controller_paths(prefix_, path),
        route_endpoint::response_stream(callback_access::invoke(handler), body_mode),
        callback_access::target(handler), middlewares_, middlewares);
}

void detail::controller_route_builder::register_extension_method_route(std::string_view method_token,
    std::string_view path, route_handler handler,
    std::span<const controller_middleware_descriptor> middlewares) const {
    const auto body_mode = declared_request_body_mode(middlewares_, middlewares);
    router_.register_endpoint(http_known_method::unknown, method_token, join_controller_paths(prefix_, path),
        route_endpoint::buffered(callback_access::invoke(handler), body_mode), callback_access::target(handler),
        middlewares_, middlewares);
}

void detail::controller_route_builder::register_extension_method_route(std::string_view method_token,
    std::string_view path, route_stream_handler handler,
    std::span<const controller_middleware_descriptor> middlewares) const {
    const auto body_mode = declared_request_body_mode(middlewares_, middlewares);
    router_.register_endpoint(http_known_method::unknown, method_token, join_controller_paths(prefix_, path),
        route_endpoint::response_stream(callback_access::invoke(handler), body_mode),
        callback_access::target(handler), middlewares_, middlewares);
}

void detail::controller_route_builder::register_tunnel_route(std::string_view protocol,
    std::string_view target, route_stream_handler handler,
    std::span<const controller_middleware_descriptor> middlewares, http_tunnel_route_config config) const {
    const auto owned_target = protocol.empty() ? std::pmr::string(target, registration_resource())
                                               : join_controller_paths(prefix_, target);
    router_.register_endpoint(http_known_method::connect, {}, owned_target,
        route_endpoint::tunnel(router_.resource(), callback_access::invoke(handler), protocol, config),
        callback_access::target(handler), middlewares_, middlewares);
}

void detail::controller_route_builder::register_websocket_route(http_known_method method,
    std::string_view path, route_stream_handler handler,
    std::span<const controller_middleware_descriptor> middlewares,
    websocket_route_config websocket_config) const {
    router_.register_endpoint(method, {}, join_controller_paths(prefix_, path),
        route_endpoint::websocket(router_.resource(), callback_access::invoke(handler), std::move(websocket_config)),
        callback_access::target(handler), middlewares_, middlewares);
}

detail::controller_route_builder detail::controller_route_builder::create_scope(
    std::string_view prefix, std::span<const controller_middleware_descriptor> middlewares) const {
    auto merged = merge_middleware_descriptors(middlewares_, middlewares);
    return controller_route_builder(router_, join_controller_paths(prefix_, prefix),
        std::move(merged), owned_prefix_tag_type{});
}

}  // namespace ruvia
