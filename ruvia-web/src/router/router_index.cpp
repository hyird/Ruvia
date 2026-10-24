#include <algorithm>
#include <bit>

#include "ruvia/http/http_connect_udp.h"
#include "ruvia/http/http_request_target.h"
#include "ruvia/http/url_encoding.h"
#include "ruvia/web/detail/util/fnv1a.h"

#include "router/path_segments.h"
#include "router/route_table.h"

namespace ruvia {

detail::route_handler detail::resolved_route::handler() const noexcept {
    return callback_access::make<task<http_response>(context&)>(
        handler_target_, route_->endpoint().buffered()->invoke());
}

detail::route_stream_handler detail::resolved_route::stream_handler() const noexcept {
    return callback_access::make<task<void>(context&)>(handler_target_, route_->endpoint().stream_invoke());
}

detail::route_resolution detail::route_table::resolved(
    const route_entry& route, const route_match& match) const noexcept {
    return route_resolution::resolved(route, handler_targets_[route.index()], match);
}

namespace {

[[nodiscard]] bool is_routable_origin_or_asterisk_path(
    http_known_method known_method, std::string_view path) noexcept {
    // Routes and path scopes name origin paths. A generic-scheme absolute-form
    // target can carry a rootless path ("x:admin/panel") that the segment trie
    // would match like "/admin/panel" while every path-scoped middleware and
    // fallback treats it as outside its prefix, so it names no route here.
    // Only an OPTIONS asterisk-form stays, for the server-wide answer.
    //
    // Dot-segments are refused for the same reason: path scopes compare raw
    // segments, while a handler resolving "/public/../private" (a wildcard
    // capture handed to static_file, for one) reaches a resource outside the
    // scope the route was matched and guarded under. A conforming client
    // removes them before sending (RFC 3986 5.2.4).
    //
    // The rejection is terminal: the document root normalizes dot segments
    // itself, so serving "/private/./file" from it would bypass the guards of
    // the route that owns "/private/file".
    return !path.empty() &&
           (path.front() == '/' || (path == "*" && known_method == http_known_method::options)) &&
           !detail::has_dot_segment(path);
}

}  // namespace

detail::route_resolution detail::route_table::resolve(const http_request& request) const noexcept {
    // A plain CONNECT routes by its authority-form target.
    if (request.known_method() == http_known_method::connect) {
        return resolve_connect({}, request.path());
    }
    const auto path = request.path();
    if (!is_routable_origin_or_asterisk_path(request.known_method(), path)) {
        return route_resolution::reject();
    }
    // An HTTP/1.1 `Upgrade: connect-udp` request routes by its origin-form path
    // among the connect-udp tunnel routes. Without one, the Upgrade is ignored
    // (RFC 9110 §7.8) and the GET routes as usual, as a WebSocket upgrade does.
    if (is_http_connect_udp_upgrade_request(request)) {
        auto tunnel = resolve_connect("connect-udp", path);
        if (tunnel.resolved() != nullptr) {
            return tunnel;
        }
    }
    return request.known_method() == http_known_method::unknown
               ? resolve_extension_method(request.method(), path)
               : resolve(request.known_method(), path);
}

detail::route_resolution detail::route_table::resolve(route_request_view request) const noexcept {
    if (request.known_method_ == http_known_method::connect && request.extended_protocol_.empty()) {
        return resolve_connect({}, request.connect_target_);
    }
    const auto path = request.path_;
    if (!is_routable_origin_or_asterisk_path(request.known_method_, path)) {
        return route_resolution::reject();
    }
    if (request.known_method_ == http_known_method::connect) {
        if (request.extended_protocol_ == "websocket") {
            auto websocket = resolve(http_known_method::get, path);
            const auto* route = websocket.resolved();
            return route != nullptr && route->route().endpoint().websocket() != nullptr
                       ? websocket
                       : route_resolution::reject();
        }
        return resolve_connect(request.extended_protocol_, path);
    }
    return request.known_method_ == http_known_method::unknown
               ? resolve_extension_method(request.method_, path)
               : resolve(request.known_method_, path);
}

detail::route_resolution detail::route_table::resolve_connect(std::string_view protocol, std::string_view target) const noexcept {
    const route_entry* fallback = nullptr;
    for (const auto index : plan_->connect_route_indices_) {
        const auto& route = plan_->routes_[index];
        if (route.endpoint().tunnel()->protocol() != protocol) {
            continue;
        }
        if (!route.dynamic() && (protocol.empty() ? http_authorities_equal(route.path(), target, 0) : uri_paths_equivalent(route.path(), target))) {
            return resolved(route);
        }
        if (protocol.empty() && route.path() == "*") {
            fallback = &route;
        }
    }
    if (!protocol.empty()) {
        for (const auto& index : plan_->connect_protocols_) {
            if (index.protocol_ != protocol) {
                continue;
            }
            route_match match;
            const auto found = find_dynamic_node(index.root_, target, match);
            if (found != no_route_index) {
                return resolved(plan_->routes_[found], match);
            }
            break;
        }
    }
    return fallback == nullptr ? route_resolution{} : resolved(*fallback);
}

detail::route_resolution detail::route_table::resolve_extension_method(
    std::string_view method_token, std::string_view path) const noexcept {
    // Not registered anywhere means the server does not know this method, which
    // is 501 and not this function's business -- dispatch decides that before
    // asking about any particular resource.
    if (!recognizes_method_token(method_token)) {
        return route_resolution{};
    }
    for (const auto index : plan_->extension_route_indices_) {
        const auto& route = plan_->routes_[index];
        // RFC 9110 9.1: the method token is case-sensitive.
        if (route.method_token() == method_token && uri_paths_equivalent(route.path(), path)) {
            return resolved(route);
        }
    }

    // The path exists under other methods, so this is 405 rather than 404. The
    // mask cannot carry extension tokens; the resolution carries them for Allow.
    const auto hash = plan_->static_method_mask_ != 0 ? path_hash(path) : 0;
    auto method_mask = allowed_methods(path, http_known_method::unknown, hash);
    const auto extension_methods = extension_methods_for(path);
    if (method_mask != 0 || !extension_methods.empty()) {
        method_mask |= 1U << method_index(http_known_method::options);
    }
    return route_resolution::method_not_allowed(method_mask, extension_methods);
}

bool detail::route_table::recognizes_method_token(std::string_view method_token) const noexcept {
    for (const auto index : plan_->extension_route_indices_) {
        if (plan_->routes_[index].method_token() == method_token) {
            return true;
        }
    }
    return false;
}

std::span<const std::string_view> detail::route_table::extension_methods_for(std::string_view path) const noexcept {
    for (const auto& entry : plan_->extension_paths_) {
        if (uri_paths_equivalent(entry.path_, path)) {
            return entry.methods_;
        }
    }
    return {};
}

detail::route_resolution detail::route_table::resolve(
    http_known_method method, std::string_view path) const noexcept {
    // RFC 9110 7.1 / 9.3.7: the asterisk-form target ("OPTIONS *") applies to the
    // server as a whole, not any resource, so it must not bind to a route -- a
    // catch-all such as RUVIA_ALL("/*") would otherwise capture it through the
    // wildcard node. Leave it unresolved so dispatch emits the server-wide response.
    if (method == http_known_method::options && path == "*") {
        return route_resolution{};
    }
    const bool has_static_method = is_routable_method(method) &&
                                   (plan_->static_method_mask_ & (1U << method_index(method))) != 0;
    auto hash = has_static_method ? path_hash(path) : 0;
    if (has_static_method) {
        if (const auto* route = find_static_route(method, path, hash); route != nullptr) {
            return resolved(*route);
        }
    }

    route_match match;
    const auto* dynamic_route = find_dynamic_route(method, path, match);
    if (dynamic_route != nullptr) {
        return resolved(*dynamic_route, match);
    }

    if (!has_static_method && plan_->static_method_mask_ != 0) {
        hash = path_hash(path);
    }
    if (method == http_known_method::head) {
        const auto* fallback = (plan_->static_method_mask_ & (1U << method_index(http_known_method::get))) != 0
                                   ? find_static_route(http_known_method::get, path, hash)
                                   : nullptr;
        if (fallback == nullptr) {
            fallback = find_dynamic_route(http_known_method::get, path, match);
        }
        if (fallback != nullptr && supports_head_fallback(*fallback)) {
            return resolved(*fallback, match);
        }
    }
    auto method_mask = allowed_methods(path, method, hash);
    // A resource may be served only by extension methods. It still exists, so
    // an unsupported known method is 405 and OPTIONS must answer with Allow --
    // the mask alone cannot tell that apart from no resource at all.
    const auto extension_methods = extension_methods_for(path);
    if (method_mask != 0 || !extension_methods.empty()) {
        method_mask |= 1U << method_index(http_known_method::options);
    }
    return route_resolution::method_not_allowed(method_mask, extension_methods);
}

std::size_t detail::route_table::method_index(http_known_method method) noexcept {
    return static_cast<std::size_t>(method);
}

bool detail::route_table::is_routable_method(http_known_method method) noexcept {
    return method_index(method) < routable_method_count;
}

bool detail::route_table::supports_head_fallback(const route_entry& route) noexcept {
    // RFC 9110 9.3.2: HEAD is GET without content. A buffered or
    // response-stream (including SSE) GET handler serves it unchanged: the
    // response plan suppresses the body and the stream writer finishes the
    // response at the handler's first body write. A websocket GET route is an
    // upgrade, not a representation, so HEAD needs its own route there.
    const auto& endpoint = route.endpoint();
    return route.method() == http_known_method::get &&
           (endpoint.buffered() != nullptr || endpoint.response_stream() != nullptr);
}

std::uint64_t detail::route_table::path_hash(std::string_view path) noexcept {
    // FNV-1a over the canonical octets (uri_paths_equivalent), so every
    // spelling of one path hashes alike without decoding into storage. A
    // literal '/' and a decoded "%2F" hash alike; only equality tells them apart.
    auto hash = detail::fnv1a_offset_basis;
    for (std::size_t i = 0; i < path.size();) {
        const auto unit = path[i] == '%' ? next_uri_path_unit(path, i) : static_cast<unsigned char>(path[i++]);
        hash ^= static_cast<unsigned char>(unit);
        hash *= detail::fnv1a_prime;
    }

    // MurmurHash3's public-domain fmix64 spreads common path prefixes across
    // the low bits used by the power-of-two table.
    hash ^= hash >> 33;
    hash *= 0xff51afd7ed558ccdULL;
    hash ^= hash >> 33;
    hash *= 0xc4ceb9fe1a85ec53ULL;
    hash ^= hash >> 33;
    return hash;
}

std::uint64_t detail::route_table::route_hash(
    http_known_method method, std::uint64_t hash) noexcept {
    return hash ^ (static_cast<std::uint64_t>(method) * detail::fnv1a_prime);
}

const detail::route_entry* detail::route_table::find_static_route(
    http_known_method method, std::string_view path, std::uint64_t hash) const noexcept {
    const auto key_hash = route_hash(method, hash);
    auto slot_index = static_cast<std::size_t>(key_hash) & plan_->static_slot_mask_;
    for (;;) {
        const auto& slot = plan_->static_slots_[slot_index];
        if (slot.route_index_ == no_route_index) {
            return nullptr;
        }
        const auto& route = plan_->routes_[slot.route_index_];
        if (slot.hash_ == key_hash && route.method() == method &&
            (route.path() == path || uri_paths_equivalent(route.path(), path))) {
            return &route;
        }
        slot_index = (slot_index + 1) & plan_->static_slot_mask_;
    }
}

std::uint32_t detail::route_table::allowed_methods(
    std::string_view path, http_known_method requested_method, std::uint64_t hash) const noexcept {
    std::uint32_t mask = 0;
    auto candidate_mask = plan_->static_method_mask_ | plan_->dynamic_method_mask_;
    // Keep OPTIONS in the candidate mask: a path whose only registered method is
    // OPTIONS must yield a non-zero Allow set so resolve() answers 405 (method known
    // but unsupported, RFC 9110 15.5.6) instead of 404. Clearing it here made such a
    // path resolve to not-found. resolve() still ORs OPTIONS into any non-zero mask,
    // so GET-only paths keep advertising OPTIONS.
    if (is_routable_method(requested_method)) {
        candidate_mask &= ~(1U << method_index(requested_method));
    }

    while (candidate_mask != 0) {
        const auto i = static_cast<std::size_t>(std::countr_zero(candidate_mask));
        const auto method = static_cast<http_known_method>(i);
        const auto method_bit = 1U << i;
        candidate_mask &= ~method_bit;

        // resolve() already proved requested_method has no route for this path, so it
        // was cleared from the candidate mask above.
        const bool has_static_routes = (plan_->static_method_mask_ & method_bit) != 0;
        const auto* const static_route = has_static_routes ? find_static_route(method, path, hash) : nullptr;
        const auto dynamic_route_index = static_route == nullptr && (plan_->dynamic_method_mask_ & method_bit) != 0
                                             ? find_dynamic_node_no_params(plan_->dynamic_roots_[i], path)
                                             : no_route_index;
        const auto* route = static_route != nullptr ? static_route
                                                    : (dynamic_route_index != no_route_index ? &plan_->routes_[dynamic_route_index] : nullptr);
        if (route != nullptr) {
            mask |= 1U << i;
            if (supports_head_fallback(*route)) {
                mask |= 1U << method_index(http_known_method::head);
            }
        }
    }
    return mask;
}

}  // namespace ruvia
