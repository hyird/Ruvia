#include <algorithm>
#include <bit>

#include "ruvia/http/http_connect_udp.h"
#include "ruvia/http/http_request_target.h"

#include "router/route_table.h"

namespace ruvia {
namespace {

constexpr std::uint64_t fnv_offset = 1469598103934665603ULL;
constexpr std::uint64_t fnv_prime = 1099511628211ULL;

}  // namespace

detail::route_resolution detail::route_table::resolve(const http_request& request) const noexcept {
    // HTTP/1 Upgrade detection remains in its adapter; only normalized routing
    // facts enter the shared classification policy.
    if (is_http_connect_udp_upgrade_request(request)) {
        return resolve(route_request_view{http_known_method::connect, request.method(),
            request.path(), request.path(), "connect-udp"});
    }
    return resolve(route_request_view{request.known_method(), request.method(),
        request.path(), request.path(), {}});
}

detail::route_resolution detail::route_table::resolve(route_request_view request) const noexcept {
    if (request.known_method_ == http_known_method::connect) {
        if (request.extended_protocol_ == "websocket") {
            return request.path_.empty() ? route_resolution{} : resolve(http_known_method::get, request.path_);
        }
        return resolve_connect(request.extended_protocol_,
            request.extended_protocol_.empty() ? request.authority_ : request.path_);
    }
    if (request.path_.empty()) {
        return {};
    }
    if (request.known_method_ == http_known_method::unknown) {
        return resolve_extension_method(request.method_token_, request.path_);
    }
    return resolve(request.known_method_, request.path_);
}

detail::route_resolution detail::route_table::resolve_connect(std::string_view protocol, std::string_view target) const noexcept {
    const route_entry* fallback = nullptr;
    for (const auto index : plan_->connect_route_indices_) {
        const auto& route = routes_[index];
        if (route.endpoint().tunnel()->protocol() != protocol) {
            continue;
        }
        if (!route.dynamic() && (protocol.empty() ? http_authorities_equal(route.path(), target, 0) : route.path() == target)) {
            return route_resolution::resolved(route);
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
                return route_resolution::resolved(routes_[found], match);
            }
            break;
        }
    }
    return fallback == nullptr ? route_resolution{} : route_resolution::resolved(*fallback);
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
        const auto& route = routes_[index];
        // RFC 9110 9.1: the method token is case-sensitive.
        if (route.method_token() == method_token && route.path() == path) {
            return route_resolution::resolved(route);
        }
    }

    // The path exists under other methods, so this is 405 rather than 404. The
    // mask cannot carry extension tokens; dispatch adds them to Allow from
    // extension_methods_for().
    const auto hash = plan_->static_method_mask_ != 0 ? path_hash(path) : 0;
    auto method_mask = allowed_methods(path, http_known_method::unknown, hash);
    const bool extension_routes = has_extension_routes_for(path);
    if (method_mask != 0 || extension_routes) {
        method_mask |= 1U << method_index(http_known_method::options);
    }
    return route_resolution::method_not_allowed(method_mask, extension_routes);
}

bool detail::route_table::recognizes_method_token(std::string_view method_token) const noexcept {
    for (const auto index : plan_->extension_route_indices_) {
        if (routes_[index].method_token() == method_token) {
            return true;
        }
    }
    return false;
}

bool detail::route_table::has_extension_routes_for(std::string_view path) const noexcept {
    for (const auto index : plan_->extension_route_indices_) {
        if (routes_[index].path() == path) {
            return true;
        }
    }
    return false;
}

std::span<const std::string_view> detail::route_table::extension_methods_for(
    std::string_view path, std::span<std::string_view> buffer) const noexcept {
    std::size_t count = 0;
    for (const auto index : plan_->extension_route_indices_) {
        if (count == buffer.size()) {
            break;
        }
        const auto& route = routes_[index];
        if (route.path() != path) {
            continue;
        }
        bool duplicate = false;
        for (std::size_t i = 0; i < count; ++i) {
            if (buffer[i] == route.method_token()) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            buffer[count++] = route.method_token();
        }
    }
    return buffer.first(count);
}

std::span<const std::string_view> detail::route_table::extension_methods_for_server() const noexcept {
    return server_extension_method_tokens_;
}

detail::route_resolution detail::route_table::resolve(
    http_known_method method, std::string_view path) const noexcept {
    if (method == http_known_method::connect) {
        return resolve_connect({}, path);
    }
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
            return route_resolution::resolved(*route);
        }
    }

    route_match match;
    const auto* dynamic_route = find_dynamic_route(method, path, match);
    if (dynamic_route != nullptr) {
        return route_resolution::resolved(*dynamic_route, std::move(match));
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
            return route_resolution::resolved(*fallback, std::move(match));
        }
    }
    auto method_mask = allowed_methods(path, method, hash);
    // A resource may be served only by extension methods. It still exists, so
    // an unsupported known method is 405 and OPTIONS must answer with Allow --
    // the mask alone cannot tell that apart from no resource at all.
    const bool extension_routes = has_extension_routes_for(path);
    if (method_mask != 0 || extension_routes) {
        method_mask |= 1U << method_index(http_known_method::options);
    }
    return route_resolution::method_not_allowed(method_mask, extension_routes);
}

std::size_t detail::route_table::method_index(http_known_method method) noexcept {
    return static_cast<std::size_t>(method);
}

bool detail::route_table::is_routable_method(http_known_method method) noexcept {
    return method_index(method) < routable_method_count;
}

bool detail::route_table::supports_head_fallback(const route_entry& route) noexcept {
    // Streaming, SSE and websocket endpoints require an explicit HEAD handler;
    // the ordinary GET handler borrows the original HEAD request unchanged.
    return route.method() == http_known_method::get && route.endpoint().buffered() != nullptr;
}

std::uint64_t detail::route_table::path_hash(std::string_view path) noexcept {
    auto hash = fnv_offset;

    for (const unsigned char c : path) {
        hash ^= c;
        hash *= fnv_prime;
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
    return hash ^ (static_cast<std::uint64_t>(method) * fnv_prime);
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
        const auto& route = routes_[slot.route_index_];
        if (slot.hash_ == key_hash && route.method() == method && route.path() == path) {
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
                                                    : (dynamic_route_index != no_route_index ? &routes_[dynamic_route_index] : nullptr);
        if (route != nullptr) {
            mask |= 1U << i;
            if (supports_head_fallback(*route)) {
                mask |= 1U << method_index(http_known_method::head);
            }
        }
    }
    return mask;
}

std::uint32_t detail::route_table::allowed_methods_for_server() const noexcept {
    return plan_->allowed_method_mask_;
}

}  // namespace ruvia
