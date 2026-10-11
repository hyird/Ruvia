#include "ruvia/web/detail/util/registration_resource.h"

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

template <typename base_range_type, typename extra_range_type>
[[nodiscard]] std::pmr::vector<controller_middleware_descriptor> merge_middleware_descriptors(
    const base_range_type& base, const extra_range_type& extra) {
    std::pmr::vector<controller_middleware_descriptor> middlewares(registration_resource());
    middlewares.reserve(base.size() + extra.size());
    middlewares.insert(middlewares.end(), base.begin(), base.end());
    middlewares.insert(middlewares.end(), extra.begin(), extra.end());
    return middlewares;
}

[[nodiscard]] std::pmr::vector<controller_middleware_descriptor> normalize_controller_middlewares(
    std::pmr::vector<controller_middleware_descriptor> middlewares) {
    if (middlewares.get_allocator().resource() == registration_resource()) {
        return middlewares;
    }

    std::pmr::vector<controller_middleware_descriptor> normalized(registration_resource());
    normalized.insert(normalized.end(), middlewares.begin(), middlewares.end());
    return normalized;
}

}  // namespace

void detail::controller_route_builder::impl_deleter_type::operator()(impl_type* impl) const noexcept {
    destroy_pmr_object(impl, registration_resource());
}

detail::controller_route_builder::controller_route_builder(router& router_value, std::string_view prefix,
    std::pmr::vector<controller_middleware_descriptor> middlewares)
    : controller_route_builder(
          router_value, join_controller_paths({}, prefix), std::move(middlewares), owned_prefix_tag_type{}) {}

detail::controller_route_builder::controller_route_builder(router& router_value, std::pmr::string prefix,
    std::pmr::vector<controller_middleware_descriptor> middlewares, owned_prefix_tag_type)
    : impl_(construct_pmr_object<impl_type>(registration_resource(), router_value, std::move(prefix),
          normalize_controller_middlewares(std::move(middlewares)))) {}

detail::controller_route_builder::controller_route_builder(controller_route_builder&&) noexcept = default;

detail::controller_route_builder& detail::controller_route_builder::operator=(
    controller_route_builder&&) noexcept = default;

detail::controller_route_builder::~controller_route_builder() = default;

void detail::controller_route_builder::register_route(http_known_method method, std::string_view path,
    route_handler_type handler, request_body_mode body_mode,
    std::span<const controller_middleware_descriptor> middlewares) const {
    router_impl::from(impl_->router())
        .register_endpoint(method, {}, join_controller_paths(impl_->prefix(), path),
            route_endpoint::buffered(callback_access::invoke(handler), body_mode), callback_access::target(handler),
            impl_->middlewares(), middlewares);
}

void detail::controller_route_builder::register_extension_method_route(std::string_view method_token,
    std::string_view path, route_handler_type handler, request_body_mode body_mode,
    std::span<const controller_middleware_descriptor> middlewares) const {
    router_impl::from(impl_->router())
        .register_endpoint(http_known_method::unknown, method_token, join_controller_paths(impl_->prefix(), path),
            route_endpoint::buffered(callback_access::invoke(handler), body_mode), callback_access::target(handler),
            impl_->middlewares(), middlewares);
}

void detail::controller_route_builder::register_response_stream_route(http_known_method method,
    std::string_view path, route_stream_handler_type handler,
    std::span<const controller_middleware_descriptor> middlewares) const {
    router_impl::from(impl_->router())
        .register_endpoint(method, {}, join_controller_paths(impl_->prefix(), path),
            route_endpoint::response_stream(callback_access::invoke(handler), http_response_stream_kind::generic),
            callback_access::target(handler), impl_->middlewares(), middlewares);
}

void detail::controller_route_builder::register_sse_route(http_known_method method, std::string_view path,
    route_stream_handler_type handler, std::span<const controller_middleware_descriptor> middlewares) const {
    router_impl::from(impl_->router())
        .register_endpoint(method, {}, join_controller_paths(impl_->prefix(), path),
            route_endpoint::response_stream(callback_access::invoke(handler), http_response_stream_kind::sse),
            callback_access::target(handler), impl_->middlewares(), middlewares);
}

void detail::controller_route_builder::register_tunnel_route(std::string_view protocol,
    std::string_view target, route_stream_handler_type handler,
    std::span<const controller_middleware_descriptor> middlewares, http_tunnel_route_config config) const {
    auto& routes = router_impl::from(impl_->router());
    const auto owned_target = protocol.empty() ? std::pmr::string(target, registration_resource())
                                               : join_controller_paths(impl_->prefix(), target);
    routes.register_endpoint(http_known_method::connect, {}, owned_target,
        route_endpoint::tunnel(routes.resource(), callback_access::invoke(handler), protocol, config),
        callback_access::target(handler), impl_->middlewares(), middlewares);
}

void detail::controller_route_builder::register_websocket_route(http_known_method method,
    std::string_view path, route_stream_handler_type handler,
    std::span<const controller_middleware_descriptor> middlewares,
    websocket_route_config websocket_config) const {
    auto& routes = router_impl::from(impl_->router());
    routes.register_endpoint(method, {}, join_controller_paths(impl_->prefix(), path),
        route_endpoint::get_websocket(routes.resource(), callback_access::invoke(handler), std::move(websocket_config)),
        callback_access::target(handler), impl_->middlewares(), middlewares);
}

detail::controller_route_builder detail::controller_route_builder::create_scope(std::string_view prefix,
    const std::pmr::vector<controller_middleware_descriptor>& middlewares) const {
    auto merged = merge_middleware_descriptors(impl_->middlewares(), middlewares);
    return controller_route_builder(impl_->router(), join_controller_paths(impl_->prefix(), prefix),
        std::move(merged), owned_prefix_tag_type{});
}

}  // namespace ruvia
