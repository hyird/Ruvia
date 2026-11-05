#include <array>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/web/streaming.h"

#include "router/router_impl.h"

// The route-registration surface a controller sees. Each call builds the
// endpoint its route kind runs and hands it, with the scope's prefix and
// middlewares, to the router's single registration path.

namespace ruvia {

using namespace detail;

namespace {

[[nodiscard]] std::size_t write_joined_controller_path(
    char* destination, std::string_view prefix, std::string_view path) noexcept {
    std::size_t length = 0;
    if (!prefix.empty() && prefix != "/") {
        if (prefix.front() != '/') {
            destination[length++] = '/';
        }
        prefix.copy(destination + length, prefix.size());
        length += prefix.size();
        if (length > 1 && destination[length - 1] == '/') {
            --length;
        }
    }
    if (path.empty() || path == "/") {
        if (length == 0) {
            destination[length++] = '/';
        }
        return length;
    }
    if (path.front() == '/') {
        path.remove_prefix(1);
    }
    destination[length++] = '/';
    path.copy(destination + length, path.size());
    return length + path.size();
}

struct scoped_controller_path final {
    static constexpr std::size_t inline_capacity = 256;
    std::array<char, inline_capacity> inline_{};
    std::pmr::string heap_{process_resource()};
    std::string_view view_{};

    scoped_controller_path(std::string_view prefix, std::string_view path) {
        if (prefix.empty() || prefix == "/") {
            if (path.empty()) {
                view_ = "/";
                return;
            }
            if (path.front() == '/') {
                view_ = path;
                return;
            }
        }
        const std::size_t max_size = prefix.size() + path.size() + 2U;
        if (max_size <= inline_capacity) {
            const auto length = write_joined_controller_path(inline_.data(), prefix, path);
            view_ = std::string_view(inline_.data(), length);
            return;
        }
        heap_.resize(max_size);
        heap_.resize(write_joined_controller_path(heap_.data(), prefix, path));
        view_ = heap_;
    }
};

[[nodiscard]] std::pmr::string join_controller_paths(std::string_view prefix, std::string_view path) {
    scoped_controller_path joined(prefix, path);
    if (!joined.heap_.empty()) {
        return std::move(joined.heap_);
    }
    return std::pmr::string(joined.view_, process_resource());
}
[[nodiscard]] std::pmr::vector<controller_middleware_descriptor> merge_middleware_descriptors(
    std::span<const controller_middleware_descriptor> base, std::span<const controller_middleware_descriptor> extra) {
    std::pmr::vector<controller_middleware_descriptor> middlewares(process_resource());
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
    const scoped_controller_path joined(prefix_, path);
    router_.register_endpoint(method, {}, joined.view_,
        route_endpoint::buffered(callback_access::invoke(handler), body_mode), callback_access::target(handler),
        middlewares_, middlewares);
}

void detail::controller_route_builder::register_route(http_known_method method, std::string_view path,
    route_stream_handler handler,
    std::span<const controller_middleware_descriptor> middlewares) const {
    const auto body_mode = declared_request_body_mode(middlewares_, middlewares);
    const scoped_controller_path joined(prefix_, path);
    router_.register_endpoint(method, {}, joined.view_,
        route_endpoint::response_stream(callback_access::invoke(handler), body_mode),
        callback_access::target(handler), middlewares_, middlewares);
}

void detail::controller_route_builder::register_extension_method_route(std::string_view method_token,
    std::string_view path, route_handler handler,
    std::span<const controller_middleware_descriptor> middlewares) const {
    const auto body_mode = declared_request_body_mode(middlewares_, middlewares);
    const scoped_controller_path joined(prefix_, path);
    router_.register_endpoint(http_known_method::unknown, method_token, joined.view_,
        route_endpoint::buffered(callback_access::invoke(handler), body_mode), callback_access::target(handler),
        middlewares_, middlewares);
}

void detail::controller_route_builder::register_extension_method_route(std::string_view method_token,
    std::string_view path, route_stream_handler handler,
    std::span<const controller_middleware_descriptor> middlewares) const {
    const auto body_mode = declared_request_body_mode(middlewares_, middlewares);
    const scoped_controller_path joined(prefix_, path);
    router_.register_endpoint(http_known_method::unknown, method_token, joined.view_,
        route_endpoint::response_stream(callback_access::invoke(handler), body_mode),
        callback_access::target(handler), middlewares_, middlewares);
}

void detail::controller_route_builder::register_tunnel_route(std::string_view protocol,
    std::string_view target, route_stream_handler handler,
    std::span<const controller_middleware_descriptor> middlewares, http_tunnel_route_config config) const {
    if (protocol.empty()) {
        router_.register_endpoint(http_known_method::connect, {}, target,
            route_endpoint::tunnel(router_.resource(), callback_access::invoke(handler), protocol, config),
            callback_access::target(handler), middlewares_, middlewares);
        return;
    }
    const scoped_controller_path joined(prefix_, target);
    router_.register_endpoint(http_known_method::connect, {}, joined.view_,
        route_endpoint::tunnel(router_.resource(), callback_access::invoke(handler), protocol, config),
        callback_access::target(handler), middlewares_, middlewares);
}

void detail::controller_route_builder::register_websocket_route(http_known_method method,
    std::string_view path, route_stream_handler handler,
    std::span<const controller_middleware_descriptor> middlewares,
    websocket_route_config websocket_config) const {
    const scoped_controller_path joined(prefix_, path);
    router_.register_endpoint(method, {}, joined.view_,
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
