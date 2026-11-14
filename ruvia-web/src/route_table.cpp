#include "route_table.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "blocking_pool.h"
#include "callback_ref.h"
#include "context.h"
#include "context_access.h"
#include "controller.h"
#include "failure_report.h"
#include "http_connect_udp.h"
#include "http_content_coding.h"
#include "http_error.h"
#include "http_message.h"
#include "http_request.h"
#include "http_request_body_failure.h"
#include "http_response.h"
#include "inbound_buffer.h"
#include "middleware.h"
#include "rate_limit.h"
#include "route_path_segment.h"
#include "session.h"
#include "static_root_index.h"
#include "streaming.h"
#include "task.h"
#include "transport_stream_end.h"
#include "url_encoding.h"

namespace ruvia {

using namespace detail;

detail::route_table::route_table(const compiled_route_plan& plan, const controller_store& controllers) :
    route_table(plan) {
    handler_targets_.reserve(plan.routes_.size());
    for (const auto& route : plan.routes_) {
        handler_targets_.push_back(controllers.instance(route.controller()));
    }
    build_middleware_instances();
}

detail::route_table::route_table(const compiled_route_plan& plan, std::span<void* const> targets) :
    route_table(plan) {
    handler_targets_.reserve(plan.routes_.size());
    for (const auto& route : plan.routes_) {
        if (route.controller() >= targets.size() || targets[route.controller()] == nullptr) {
            throw std::invalid_argument("route handler slot is not bound");
        }
        handler_targets_.push_back(targets[route.controller()]);
    }
    build_middleware_instances();
}

detail::route_table::route_table(const compiled_route_plan& plan) :
    plan_(&plan),
    handler_targets_(),
    middleware_instances_(),
    middleware_frames_() {}

void detail::route_table::build_middleware_instances() {
    const auto& plan = *plan_;

    middleware_instances_.reserve(plan.middleware_instances_.size());
    middleware_frames_.reserve(plan.middleware_frame_instances_.size());
    try {
        for (const auto& descriptor : plan.middleware_instances_) {
            middleware_instances_.push_back(descriptor.create()(descriptor.args()));
        }
    } catch (...) {
        for (std::size_t i = middleware_instances_.size(); i != 0; --i) {
            plan.middleware_instances_[i - 1].destroy()(middleware_instances_[i - 1]);
        }
        middleware_instances_.clear();
        throw;
    }

    for (const auto instance : plan.middleware_frame_instances_) {
        middleware_frames_.push_back(callback_access::make<task<void>(context&, next&)>(middleware_instances_[instance], plan.middleware_instances_[instance].invoke()));
    }
}

detail::route_table::~route_table() {
    for (std::size_t i = middleware_instances_.size(); i != 0; --i) {
        plan_->middleware_instances_[i - 1].destroy()(middleware_instances_[i - 1]);
    }
}

void detail::route_table::swap(route_table& other) noexcept {
    std::swap(plan_, other.plan_);
    handler_targets_.swap(other.handler_targets_);
    middleware_instances_.swap(other.middleware_instances_);
    middleware_frames_.swap(other.middleware_frames_);
}

// Substitute parameters and percent-encode path values while preserving the
// same segments used to build and resolve the registered route.

namespace {

// RFC 3986 pchar minus pct-encoded: bytes a path segment may carry verbatim.
[[nodiscard]] constexpr bool is_url_for_segment_byte(char value) noexcept {
    const auto byte = static_cast<unsigned char>(value);
    return is_uri_unreserved_byte(byte) || value == '!' || value == '$' || value == '&' || value == '\'' || value == '(' || value == ')' || value == '*' || value == '+' || value == ',' || value == ';' || value == '=' || value == ':' || value == '@';
}

[[nodiscard]] constexpr std::size_t url_for_encoded_size(std::string_view value, bool keep_slashes) noexcept {
    std::size_t size = value.size();
    for (const char byte : value) {
        if (!is_url_for_segment_byte(byte) && !(keep_slashes && byte == '/')) {
            size += 2U;
        }
    }
    return size;
}

void append_url_for_value(std::string& output, std::string_view value, bool keep_slashes) {
    while (!value.empty()) {
        const auto escaped = std::ranges::find_if_not(value, [keep_slashes](char byte) noexcept {
            return is_url_for_segment_byte(byte) || (keep_slashes && byte == '/');
        });
        const auto plain_size = static_cast<std::size_t>(escaped - value.begin());
        if (plain_size != 0) {
            output.append(value.data(), plain_size);
        }
        if (escaped == value.end()) {
            return;
        }
        http_append_percent_byte(output, static_cast<unsigned char>(*escaped));
        value.remove_prefix(plain_size + 1);
    }
}

// RFC 3986 5.2.4 resolvers remove "." and ".." segments, and WHATWG URL
// parsers treat "%2E" spellings the same way, so encoding cannot keep them.
[[nodiscard]] constexpr bool is_url_for_dot_segment(std::string_view segment) noexcept {
    return segment == "." || segment == "..";
}

[[nodiscard]] bool has_url_for_dot_segment(std::string_view value) noexcept {
    while (true) {
        const auto slash = value.find('/');
        if (is_url_for_dot_segment(value.substr(0, slash))) {
            return true;
        }
        if (slash == std::string_view::npos) {
            return false;
        }
        value.remove_prefix(slash + 1);
    }
}

}  // namespace

std::string detail::route_table::url_for(std::string_view pattern, std::span<const std::string_view> values) const {
    if (!std::ranges::binary_search(plan_->url_patterns_, pattern)) {
        throw std::invalid_argument("url_for pattern is not a registered route");
    }

    // Validate every segment and value before allocating, and size the buffer
    // from the actual parameter values so short URLs stay in SSO and rejected
    // calls never touch a monotonic arena.
    std::size_t next_value = 0;
    std::size_t estimated_size = 0;
    auto remaining = pattern;
    std::string_view segment;
    std::string_view rest;
    while (detail::split_path_segment(remaining, segment, rest)) {
        remaining = rest;
        const auto kind = classify_route_segment(segment);
        if (kind == route_segment_kind::wildcard && remaining.empty()) {
            if (next_value >= values.size()) {
                throw std::invalid_argument("url_for is missing a value for '*'");
            }
            const auto value = values[next_value++];
            if (has_url_for_dot_segment(value)) {
                throw std::invalid_argument("url_for '*' value must not contain a '.' or '..' segment");
            }
            if (value.find('\\') != std::string_view::npos || (estimated_size == 0 && value.starts_with('/'))) {
                throw std::invalid_argument("url_for '*' value has a separator the route cannot capture");
            }
            if (!value.empty()) {
                estimated_size += 1U + url_for_encoded_size(value, /*keep_slashes=*/true);
            } else if (estimated_size == 0) {
                estimated_size += 1U;
            }
            continue;
        }
        if (estimated_size == 0 && segment.empty()) {
            throw std::invalid_argument("url_for pattern must not start with an empty segment");
        }
        estimated_size += 1U;
        if (kind == route_segment_kind::param) {
            if (next_value >= values.size()) {
                throw std::invalid_argument("url_for is missing a route parameter value");
            }
            const auto value = values[next_value++];
            if (value.empty()) {
                throw std::invalid_argument("url_for route parameter value must not be empty");
            }
            if (is_url_for_dot_segment(value)) {
                throw std::invalid_argument("url_for route parameter value must not be '.' or '..'");
            }
            estimated_size += url_for_encoded_size(value, /*keep_slashes=*/false);
        } else {
            estimated_size += segment.size();
        }
    }
    if (next_value != values.size()) {
        throw std::invalid_argument("url_for received more values than the pattern has parameters");
    }

    std::string url;
    if (estimated_size > 15U) {
        url.reserve(estimated_size);
    }

    next_value = 0;
    remaining = pattern;
    while (detail::split_path_segment(remaining, segment, rest)) {
        remaining = rest;
        const auto kind = classify_route_segment(segment);
        if (kind == route_segment_kind::wildcard && remaining.empty()) {
            const auto value = values[next_value++];
            if (!value.empty()) {
                url.push_back('/');
                append_url_for_value(url, value, /*keep_slashes=*/true);
            } else if (url.empty()) {
                url.push_back('/');
            }
            continue;
        }
        url.push_back('/');
        if (kind == route_segment_kind::param) {
            append_url_for_value(url, values[next_value++], /*keep_slashes=*/false);
        } else {
            url.append(segment.data(), segment.size());
        }
    }
    return url;
}

detail::route_handler detail::resolved_route::handler() const noexcept {
    return callback_access::make<task<http_response>(context&)>(handler_target_, route_->endpoint().buffered()->invoke());
}

detail::route_stream_handler detail::resolved_route::stream_handler() const noexcept {
    return callback_access::make<task<void>(context&)>(handler_target_, route_->endpoint().stream_invoke());
}

detail::route_resolution detail::route_table::resolved(const route_entry& route) const noexcept {
    return route_resolution::resolved(route, handler_targets_[route.index()]);
}

detail::route_resolution detail::route_table::resolved(const route_entry& route, const route_match& match) const noexcept {
    return route_resolution::resolved(route, handler_targets_[route.index()], match);
}

namespace {

[[nodiscard]] bool is_routable_origin_or_asterisk_path(http_known_method known_method, std::string_view path) noexcept {
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
    return !path.empty() && (path.front() == '/' || (path == "*" && known_method == http_known_method::options)) && !detail::has_dot_segment(path);
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
    return request.known_method() == http_known_method::unknown ? resolve_extension_method(request.method(), path) : resolve(request.known_method(), path);
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
            return route != nullptr && route->route().endpoint().websocket() != nullptr ? websocket : route_resolution::reject();
        }
        return resolve_connect(request.extended_protocol_, path);
    }
    return request.known_method_ == http_known_method::unknown ? resolve_extension_method(request.method_, path) : resolve(request.known_method_, path);
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

detail::route_resolution detail::route_table::resolve_extension_method(std::string_view method_token, std::string_view path) const noexcept {
    // Not registered anywhere means the server does not know this method, which
    // is 501 and not this function's business -- dispatch decides that before
    // asking about any particular resource.
    const auto& indices = plan_->extension_route_indices_;
    const auto found = std::lower_bound(indices.begin(), indices.end(), std::pair{method_token, path}, [this](std::size_t index, const auto& key) noexcept {
        const auto& route = plan_->routes_[index];
        if (route.method_token() != key.first) {
            return route.method_token() < key.first;
        }
        return compare_uri_paths(route.path(), key.second) < 0;
    });
    if (found != indices.end()) {
        const auto& route = plan_->routes_[*found];
        if (route.method_token() == method_token && uri_paths_equivalent(route.path(), path)) {
            return resolved(route);
        }
    }
    if (!recognizes_method_token(method_token)) {
        return route_resolution{};
    }

    // The path exists under other methods, so this is 405 rather than 404. The
    // mask cannot carry extension tokens; the resolution carries them for Allow.
    const auto hash = plan_->static_method_mask_ != 0 ? path_hash(path) : 0;
    auto method_mask = allowed_methods(path, hash);
    const auto extension_methods = extension_methods_for(path);
    if (method_mask != 0 || !extension_methods.empty()) {
        method_mask |= 1U << method_index(http_known_method::options);
    }
    return route_resolution::method_not_allowed(method_mask, extension_methods);
}

bool detail::route_table::recognizes_method_token(std::string_view method_token) const noexcept {
    const auto& indices = plan_->extension_route_indices_;
    const auto found = std::ranges::lower_bound(indices, method_token, {}, [this](std::size_t index) noexcept {
        return plan_->routes_[index].method_token();
    });
    return found != indices.end() && plan_->routes_[*found].method_token() == method_token;
}

std::span<const std::string_view> detail::route_table::extension_methods_for(std::string_view path) const noexcept {
    const auto& paths = plan_->extension_paths_;
    const auto found = std::ranges::lower_bound(paths, path, [](std::string_view left, std::string_view right) noexcept {
        return compare_uri_paths(left, right) < 0;
    }, &compiled_route_plan::extension_path_type::path_);
    return found != paths.end() && uri_paths_equivalent(found->path_, path) ? found->methods_ : std::span<const std::string_view>{};
}

detail::route_resolution detail::route_table::resolve(http_known_method method, std::string_view path) const noexcept {
    // RFC 9110 7.1 / 9.3.7: the asterisk-form target ("OPTIONS *") applies to the
    // server as a whole, not any resource, so it must not bind to a route -- a
    // catch-all such as RUVIA_ALL("/*") would otherwise capture it through the
    // wildcard node. Leave it unresolved so dispatch emits the server-wide response.
    if (method == http_known_method::options && path == "*") {
        return route_resolution{};
    }
    const bool has_static_method = is_routable_method(method) && (plan_->static_method_mask_ & (1U << method_index(method))) != 0;
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
    std::uint32_t excluded_mask = is_routable_method(method) ? (1U << method_index(method)) : 0U;
    std::uint32_t initial_mask = 0;
    if (method == http_known_method::head) {
        const auto get_bit = 1U << method_index(http_known_method::get);
        const auto* fallback = (plan_->static_method_mask_ & get_bit) != 0 ? find_static_route(http_known_method::get, path, hash) : nullptr;
        if (fallback == nullptr) {
            fallback = find_dynamic_route(http_known_method::get, path, match);
        }
        if (fallback != nullptr && supports_head_fallback(*fallback)) {
            return resolved(*fallback, match);
        }
        excluded_mask |= get_bit;
        if (fallback != nullptr) {
            initial_mask |= get_bit;
        }
    }
    auto method_mask = allowed_methods(path, hash, excluded_mask, initial_mask);
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
    // RFC 9110 9.3.2: HEAD is GET without content. Buffered responses omit
    // content; response-stream/SSE handlers observe completed at explicit
    // commit and must not start producing a body. A websocket GET is an
    // upgrade, not a representation, so HEAD needs its own route there.
    const auto& endpoint = route.endpoint();
    return route.method() == http_known_method::get && (endpoint.buffered() != nullptr || endpoint.response_stream() != nullptr);
}

std::uint64_t detail::route_table::path_hash(std::string_view path) noexcept {
    // FNV-1a over the canonical octets (uri_paths_equivalent), so every
    // spelling of one path hashes alike without decoding into storage. A
    // literal '/' and a decoded "%2F" hash alike; only equality tells them apart.
    auto hash = fnv1a_offset_basis;
    for (std::size_t i = 0; i < path.size();) {
        const auto unit = path[i] == '%' ? next_uri_path_unit(path, i) : static_cast<unsigned char>(path[i++]);
        hash ^= static_cast<unsigned char>(unit);
        hash *= fnv1a_prime;
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

std::uint64_t detail::route_table::route_hash(http_known_method method, std::uint64_t hash) noexcept {
    return hash ^ (static_cast<std::uint64_t>(method) * fnv1a_prime);
}

const detail::route_entry* detail::route_table::find_static_route(http_known_method method, std::string_view path, std::uint64_t hash) const noexcept {
    const auto key_hash = route_hash(method, hash);
    auto slot_index = static_cast<std::size_t>(key_hash) & plan_->static_slot_mask_;
    for (;;) {
        const auto& slot = plan_->static_slots_[slot_index];
        if (slot.route_index_ == no_route_index) {
            return nullptr;
        }
        const auto& route = plan_->routes_[slot.route_index_];
        if (slot.hash_ == key_hash && route.method() == method && (route.path() == path || uri_paths_equivalent(route.path(), path))) {
            return &route;
        }
        slot_index = (slot_index + 1) & plan_->static_slot_mask_;
    }
}

std::uint32_t detail::route_table::allowed_methods(std::string_view path, std::uint64_t hash, std::uint32_t excluded_mask, std::uint32_t initial_mask) const noexcept {
    std::uint32_t mask = initial_mask;
    // Keep OPTIONS in the candidate mask: a path whose only registered method is
    // OPTIONS must yield a non-zero Allow set so resolve() answers 405 (method known
    // but unsupported, RFC 9110 15.5.6) instead of 404. Clearing it here made such a
    // path resolve to not-found. resolve() still ORs OPTIONS into any non-zero mask,
    // so GET-only paths keep advertising OPTIONS.
    auto candidate_mask = (plan_->static_method_mask_ | plan_->dynamic_method_mask_) & ~excluded_mask;
    while (candidate_mask != 0) {
        const auto i = static_cast<std::size_t>(std::countr_zero(candidate_mask));
        const auto method = static_cast<http_known_method>(i);
        const auto method_bit = 1U << i;
        candidate_mask &= ~method_bit;

        // resolve() already proved requested_method has no route for this path, so it
        // was cleared from the candidate mask above.
        const bool has_static_routes = (plan_->static_method_mask_ & method_bit) != 0;
        const auto* const static_route = has_static_routes ? find_static_route(method, path, hash) : nullptr;
        const auto dynamic_route_index = static_route == nullptr && (plan_->dynamic_method_mask_ & method_bit) != 0 ? find_dynamic_node_no_params(plan_->dynamic_roots_[i], path) : no_route_index;
        const auto* route = static_route != nullptr ? static_route : (dynamic_route_index != no_route_index ? &plan_->routes_[dynamic_route_index] : nullptr);
        if (route != nullptr) {
            mask |= 1U << i;
            if (supports_head_fallback(*route)) {
                mask |= 1U << method_index(http_known_method::head);
            }
        }
    }
    return mask;
}

std::size_t detail::route_table::find_dynamic_node(const dynamic_node_type& node_value, std::string_view path, route_match& match) noexcept {
    std::string_view segment;
    std::string_view rest;
    if (!split_path_segment(path, segment, rest)) {
        if (node_value.route_index_ != no_route_index) {
            return node_value.route_index_;
        }
        if (node_value.wildcard_route_index_ != no_route_index && match.add({})) {
            return node_value.wildcard_route_index_;
        }
        return no_route_index;
    }

    const auto original_param_count = match.size();
    if (const auto* static_child = find_dynamic_static_child(node_value, segment); static_child != nullptr) {
        if (const auto route_index = find_dynamic_node(*static_child->node_, rest, match); route_index != no_route_index) {
            return route_index;
        }
        match.truncate(original_param_count);
    }

    if (node_value.param_child_ && !segment.empty() && match.add(segment)) {
        if (const auto route_index = find_dynamic_node(*node_value.param_child_, rest, match); route_index != no_route_index) {
            return route_index;
        }
        match.truncate(original_param_count);
    }

    if (node_value.wildcard_route_index_ != no_route_index) {
        auto capture_value = path;
        if (capture_value.starts_with('/')) {
            capture_value.remove_prefix(1);
        }
        if (match.add(capture_value)) {
            return node_value.wildcard_route_index_;
        }
        match.truncate(original_param_count);
    }

    return no_route_index;
}

std::size_t detail::route_table::find_dynamic_node_no_params(const dynamic_node_type& node_value, std::string_view path) noexcept {
    std::string_view segment;
    std::string_view rest;
    if (!split_path_segment(path, segment, rest)) {
        return node_value.route_index_ != no_route_index ? node_value.route_index_ : node_value.wildcard_route_index_;
    }

    if (const auto* static_child = find_dynamic_static_child(node_value, segment); static_child != nullptr) {
        if (const auto route_index = find_dynamic_node_no_params(*static_child->node_, rest); route_index != no_route_index) {
            return route_index;
        }
    }

    if (node_value.param_child_ != nullptr && !segment.empty()) {
        if (const auto route_index = find_dynamic_node_no_params(*node_value.param_child_, rest); route_index != no_route_index) {
            return route_index;
        }
    }

    return node_value.wildcard_route_index_;
}

const detail::route_table::dynamic_static_child_type* detail::route_table::find_dynamic_static_child(const dynamic_node_type& node_value, std::string_view segment) noexcept {
    // Literal segments compare in their canonical form (uri_paths_equivalent),
    // so "%61dmin" and "admin" select the same child; children are sorted by
    // compare_uri_paths at build time.
    if (node_value.static_children_.size() <= 4) {
        for (const auto& child : node_value.static_children_) {
            if (uri_paths_equivalent(child.segment_, segment)) {
                return &child;
            }
        }
        return nullptr;
    }

    const auto iter = std::ranges::partition_point(node_value.static_children_, [segment](const dynamic_static_child_type& child_value) noexcept {
        return compare_uri_paths(child_value.segment_, segment) < 0;
    });
    if (iter != node_value.static_children_.end() && uri_paths_equivalent(iter->segment_, segment)) {
        return &*iter;
    }
    return nullptr;
}

const detail::route_entry* detail::route_table::find_dynamic_route(http_known_method method, std::string_view path, route_match& match) const noexcept {
    match.clear();
    if (!is_routable_method(method)) {
        return nullptr;
    }

    const auto method_bit = 1U << method_index(method);
    if ((plan_->dynamic_method_mask_ & method_bit) == 0) {
        return nullptr;
    }
    const auto route_index = find_dynamic_node(plan_->dynamic_roots_[method_index(method)], path, match);
    if (route_index == no_route_index || match.size() != plan_->routes_[route_index].param_names().size()) {
        match.clear();
        return nullptr;
    }
    return &plan_->routes_[route_index];
}

// Choosing what answers a request: the matched route, a 405 with Allow, the
// document-root fallback, or nothing -- and running a buffered handler once one
// is chosen.

namespace {

http_response make_allow_no_content_response(std::uint32_t method_mask, std::span<const std::string_view> extension_methods) {
    http_response response;
    response.status(ruvia::http_status::no_content);
    response.allow_methods(method_mask, extension_methods);
    return response;
}

// Methods a document-root file supports: the fallback serves GET and HEAD and
// answers OPTIONS; every other known method is 405 with this Allow set.
constexpr std::uint32_t document_root_method_mask = (1U << static_cast<unsigned>(http_known_method::get)) | (1U << static_cast<unsigned>(http_known_method::head)) | (1U << static_cast<unsigned>(http_known_method::options));

// Outcome of probing the document root for an unrouted request: nothing there,
// a complete GET/HEAD response, or an existing resource asked with another
// method, which the dispatcher answers with 405 or an OPTIONS Allow set.
struct document_root_probe final {
    std::optional<detail::static_root_entry_view> entry_;
    std::string_view relative_{};
};

[[nodiscard]] document_root_probe probe_document_root_resource(const detail::document_root_binding& document_root, const http_request& request, std::optional<std::string>& decoded, std::optional<std::string>& storage) {
    const auto* const root = document_root.root();
    // Only an origin-form path names a document-root resource: a CONNECT
    // authority or "OPTIONS *" reaches here unrouted as well and must not be
    // probed as a file name. resolve() has already rejected dot segments.
    // static_file takes a decoded file path: decode it here, once, so a name
    // holding an encoded octet ("%20", UTF-8) resolves (RFC 3986 2.1, 6.2.2.2).
    if (root == nullptr || !request.path().starts_with('/') || request.path().find("//") != std::string_view::npos) {
        return {};
    }
    const auto path = decode_url_component(request.path(), decoded.emplace(), url_decode_mode::path);
    if (!path) {
        return {};
    }
    auto relative = path->substr(1);
    auto entry_value = detail::static_root_access::try_resolve(*root, relative, storage.emplace());
    if (!entry_value.has_value()) {
        return {};
    }
    return {.entry_ = entry_value, .relative_ = relative};
}

}  // namespace

task<std::optional<http_response>> detail::route_table::dispatch_response_stream(const http_request& request, const resolved_route& resolved, response_stream_writer& response_stream, const context_services& services) const {
    if (resolved.route().endpoint().response_stream() == nullptr) {
        throw std::logic_error("route is not a response stream route");
    }
    return dispatch_stream_route(request, resolved, resolved.stream_handler(), services, &response_stream);
}

task<std::optional<http_response>> detail::route_table::dispatch_upgrade(const http_request& request, const resolved_route& resolved, const route_stream_handler& handler, const context_services& services) const {
    const auto& endpoint = resolved.route().endpoint();
    if (endpoint.tunnel() == nullptr && endpoint.websocket() == nullptr) {
        throw std::logic_error("route is not an upgrade route");
    }
    return dispatch_stream_route(request, resolved, handler, services);
}

task<void> detail::route_table::dispatch_buffered_response(const http_request& request, const route_resolution& resolution, document_root_binding document_root, const context_services& services, static_file_selection_mode static_file_mode, http_response& response) const {
    std::exception_ptr dispatch_exception;
    try {
        const auto* resolved = resolution.resolved();
        if (resolved == nullptr) {
            // A document-root file or an automatic OPTIONS answer: complete
            // responses that still pass through the unmatched-request chain,
            // like every other answer to a request no route matched. A
            // rejected target is answered by the not-found path alone.
            std::optional<std::string> decoded_fallback_path;
            std::optional<std::string> fallback_path_storage;
            const auto fallback_probe = resolution.rejected() != nullptr || document_root.root() == nullptr ? document_root_probe{} : probe_document_root_resource(document_root, request, decoded_fallback_path, fallback_path_storage);
            const auto method = request.known_method();
            if (fallback_probe.entry_.has_value() && (method == http_known_method::get || method == http_known_method::head)) {
                auto context_value = detail::context_access::make(request, services);
                detail::context_access::services(context_value).bind_routes(*this);
                auto fallback_response = co_await detail::context_access::static_file_resolved(context_value, *document_root.root(), fallback_probe.relative_, {}, *fallback_probe.entry_, static_file_mode);
                auto terminal = [&fallback_response](context&) -> task<http_response> {
                    co_return std::move(fallback_response);
                };
                response = co_await run_unmatched_chain(context_value, callback_access::bind<task<http_response>(context&)>(terminal));
                co_return;
            }
            std::optional<http_response> unmatched_response;
            // One respond_error co_await serves both rejection kinds: each
            // co_await expression reserves its own slots for the call's
            // temporaries in the frame, so distinct inline sites would each
            // add a resident http_response-sized block.
            std::optional<http_error_info> error;
            std::uint32_t allowed_methods = 0;
            std::span<const std::string_view> extension_methods;
            if (request.known_method() == http_known_method::unknown && !recognizes_method_token(request.method())) {
                // RFC 9110 15.5.6/15.6.2: 405 says the method is known here but
                // unsupported by this resource; a token no route registered is
                // not known here at all, so it stays 501 whatever the path holds.
                error = http_error_info({.status_ = ruvia::http_status::not_implemented, .message_ = "method not implemented"});
            } else if (request.known_method() == http_known_method::options && request.path() == "*") {
                // RFC 9110 9.3.7: OPTIONS * asks about the server in general.
                // Methods differ per resource, so a server-wide Allow would
                // advertise methods most resources reject; answer without one.
                unmatched_response.emplace(http_response());
                unmatched_response->status(ruvia::http_status::no_content);
            } else if (resolution.method_not_allowed() != nullptr || fallback_probe.entry_.has_value()) {
                // The method is known here (registered as an extension token
                // or built in) but this resource -- a routed path or a
                // document-root file -- does not support it. Allow names what
                // the routes register for this path plus what the file supports.
                const auto* method_not_allowed = resolution.method_not_allowed();
                if (method_not_allowed != nullptr) {
                    extension_methods = method_not_allowed->extension_methods();
                    allowed_methods = method_not_allowed->allowed_methods();
                }
                if (fallback_probe.entry_.has_value()) {
                    allowed_methods |= document_root_method_mask;
                }
                if (request.known_method() == http_known_method::options) {
                    unmatched_response.emplace(make_allow_no_content_response(allowed_methods, extension_methods));
                    allowed_methods = 0;
                    extension_methods = {};
                } else {
                    error = http_error_info({.status_ = ruvia::http_status::method_not_allowed, .message_ = "method not allowed"});
                }
            }

            if (unmatched_response) {
                auto context_value = detail::context_access::make(request, services);
                detail::context_access::services(context_value).bind_routes(*this);
                auto terminal = [&unmatched_response](context&) -> task<http_response> {
                    co_return std::move(*unmatched_response);
                };
                response = co_await run_unmatched_chain(context_value, callback_access::bind<task<http_response>(context&)>(terminal));
                co_return;
            }

            if (error) {
                response = co_await respond_error(request, *error, services);
                if (allowed_methods != 0 || !extension_methods.empty()) {
                    response.allow_methods(allowed_methods, extension_methods);
                }
                co_return;
            }

            response = co_await respond_not_found(request, services);
            co_return;
        }

        (void)co_await dispatch_resolved_route(request, *resolved, services, nullptr, response);
        co_return;
    } catch (...) {
        dispatch_exception = std::current_exception();
    }
    response = co_await respond_exception(request, dispatch_exception, services);
}

task<bool> detail::route_table::dispatch_resolved_route(const http_request& request, const resolved_route& resolved, const context_services& services, const std::optional<http_request_body_failure>* body_failure, http_response& response) const {
    const auto body_failed = [body_failure] {
        return body_failure != nullptr && body_failure->has_value();
    };
    bool body_failure_handled = false;
    std::exception_ptr dispatch_exception;
    try {
        auto context_value = make_route_context(request, resolved, services);
        auto& context_services_value = detail::context_access::services(context_value);
        context_services_value.bind_routes(*this);
        context_services_value.bind_route_body_limit(resolved.route().max_request_body_bytes());
        response = co_await dispatch_bound_buffered(resolved, context_value, body_failure);
        body_failure_handled = body_failed() && context_value.exception() != nullptr;
    } catch (...) {
        dispatch_exception = std::current_exception();
    }
    if (dispatch_exception != nullptr) {
        // Context construction and failed error finalization keep the
        // request-only fallback; transport failures still escape to the session.
        body_failure_handled = body_failed();
        response = co_await respond_exception(request, dispatch_exception, services);
    }
    co_return body_failure_handled;
}

task<http_response> detail::route_table::dispatch_bound_buffered(const resolved_route& resolved, context& context_value, const std::optional<http_request_body_failure>* body_failure) const {
    std::exception_ptr exception;
    try {
        auto response = co_await invoke_route(resolved, context_value);
        if (body_failure != nullptr && body_failure->has_value() && context_value.exception() == nullptr) {
            throw http_request_body_protocol_error(**body_failure);
        }
        if (!resolved.route().has_middleware()) {
            detail::context_access::apply_response_policy(context_value, response);
        }
        co_return response;
    } catch (...) {
        exception = std::current_exception();
    }
    co_return co_await finalize_failure(context_value, exception);
}

// Turning a failed request into a response: the error a thrown exception really
// carries, the metadata that survives onto the response, and the scoped error /
// not-found handlers that get the last word.

namespace {

// The error a thrown exception really carries, owned by the request arena so
// an application error handler may suspend while it is read.
struct classified_error final {
    http_error error_;
    std::string_view accepted_codings_{};
    std::optional<detail::rate_limit_rejection> rate_limit_rejection_{};
    // Not an HTTP-mapped failure: answered with a generic 500.
    bool unexpected_{false};
};

[[nodiscard]] classified_error classify_exception(const std::exception_ptr& exception) {
    try {
        if (exception != nullptr) {
            std::rethrow_exception(exception);
        }
        return {.error_ = http_error({.status_ = ruvia::http_status::internal_server_error, .message_ = "unhandled exception"})};
    } catch (const detail::inbound_transport_error&) {
        throw;
    } catch (const detail::unsupported_request_content_coding& error) {
        return {.error_ = http_error({.status_ = http_unsupported_content_coding::status(), .code_ = "unsupported_content_coding", .message_ = error.what()}), .accepted_codings_ = error.accepted_codings()};
    } catch (const detail::rate_limit_rejected& rejected) {
        const auto error = detail::rate_limit_rejection_error();
        return {.error_ = http_error({.status_ = error.status(), .code_ = error.code(), .message_ = error.message()}), .rate_limit_rejection_ = rejected.rejection()};
    } catch (const http_error& error) {
        return {.error_ = http_error(error)};
    } catch (const http_protocol_error& error) {
        return {.error_ = http_error({.status_ = error.status(), .code_ = error.code(), .message_ = error.what()})};
    } catch (const detail::inbound_buffer_limit_error&) {
        return {.error_ = http_error({.status_ = ruvia::http_status::service_unavailable, .code_ = "inbound_buffer_limit", .message_ = "inbound buffer capacity exhausted"})};
    } catch (const blocking_operation_rejected& error) {
        // The blocking pool refused the work or the worker is going away. That
        // is capacity, not a bug in the request: answer it like any other
        // overload instead of a 500. The message is the framework's own and
        // names no application internals.
        return {.error_ = http_error({.status_ = ruvia::http_status::service_unavailable, .code_ = "blocking_pool_unavailable", .message_ = error.what()})};
    } catch (...) {
        // An unexpected exception (e.g. a database/library error) may carry
        // internal detail -- table names, query fragments, file paths. Do NOT echo
        // what() to the client: an info without a message renders the generic
        // "Internal Server Error" body. The exception_ptr is still set on the context, so an on_error
        // handler can log or inspect the full detail server-side; without one it
        // is reported to stderr (report_unexpected_failure).
        return {.error_ = http_error({.status_ = ruvia::http_status::internal_server_error}), .unexpected_ = true};
    }
}

// An unexpected exception answered without an application error handler would
// otherwise leave no server-side trace at all.
void report_unexpected_failure(const classified_error& error_info, const std::exception_ptr& exception, detail::http_error_handler_ref error_handler) noexcept {
    if (error_info.unexpected_ && error_handler == nullptr) {
        ruvia::report_unhandled_failure("request handler", exception);
    }
}

void apply_exception_response_metadata(http_response& response, const classified_error& error_info) {
    if (!error_info.accepted_codings_.empty()) {
        response.header_stable_view("Accept-Encoding", error_info.accepted_codings_);
    } else if (error_info.rate_limit_rejection_.has_value()) {
        detail::apply_rate_limit_rejection_headers(response, *error_info.rate_limit_rejection_);
    }
}

}  // namespace

detail::default_exception_response detail::make_default_exception_response(const std::exception_ptr& exception) {
    const auto error_info = classify_exception(exception);
    auto response = make_default_error_response(error_info.error_.info());
    apply_exception_response_metadata(response, error_info);
    return {.response_ = std::move(response), .unexpected_ = error_info.unexpected_};
}

task<http_response> detail::route_table::respond_error(const http_request& request, http_error_info error, const context_services& services) const {
    // A request stream request reaches neither the error handler nor the
    // unmatched middleware: its listener serves no application route.
    const bool request_stream = static_cast<bool>(services.request_stream());
    const auto error_handler = request_stream ? nullptr : error_handler_for(request.path());
    // Nothing to wrap and no handler: the original allocation-free path.
    if (error_handler == nullptr && (request_stream || plan_->unmatched_middleware_count_ == 0)) {
        auto response = make_default_error_response(error);
        co_return response;
    }

    auto context_value = detail::context_access::make(request, services);
    detail::context_access::services(context_value).bind_routes(*this);
    auto terminal = [error, error_handler](context& terminal_context) -> task<http_response> {
        if (error_handler == nullptr) {
            co_return make_default_error_response(error);
        }
        co_return co_await invoke_error_handler(terminal_context, error, error_handler);
    };
    // Drivers call this to answer a rejection or a recovery outcome, so it must
    // always produce a response: a failure escaping the wrapping middleware
    // chain (for example a middleware that neither responds nor awaits next())
    // is answered the way dispatch answers it, without re-entering the chain.
    std::exception_ptr exception;
    try {
        auto response = co_await run_unmatched_chain(context_value, callback_access::bind<task<http_response>(context&)>(terminal));
        co_return response;
    } catch (...) {
        exception = std::current_exception();
    }
    co_return co_await finalize_failure(context_value, exception);
}

task<http_response> detail::route_table::respond_exception(const http_request& request, std::exception_ptr exception, const context_services& services) const {
    return respond_exception(request, std::move(exception), services, services.request_stream() ? nullptr : error_handler_for(request.path()));
}

task<http_response> detail::route_table::respond_exception(const http_request& request, std::exception_ptr exception, const context_services& services, http_error_handler_ref error_handler) const {
    if (error_handler == nullptr) {
        auto answer = make_default_exception_response(exception);
        if (answer.unexpected_) {
            ruvia::report_unhandled_failure("request handler", exception);
        }
        co_return std::move(answer.response_);
    }

    auto context_value = detail::context_access::make(request, services);
    detail::context_access::services(context_value).bind_routes(*this);
    auto response = co_await respond_exception(context_value, exception, error_handler);
    detail::context_access::apply_response_policy(context_value, response);
    co_return response;
}

task<http_response> detail::route_table::respond_not_found(const http_request& request, const context_services& services) const {
    const auto not_found_handler = not_found_handler_for(request.path());
    if (not_found_handler == nullptr && plan_->unmatched_middleware_count_ == 0) {
        co_return make_route_not_found_response();
    }

    auto context_value = detail::context_access::make(request, services);
    detail::context_access::services(context_value).bind_routes(*this);
    // The handler's own failure keeps going through respond_exception, exactly as
    // before; wrapping it in the chain must not change which layer answers it.
    auto terminal = [this, not_found_handler](context& terminal_context) -> task<http_response> {
        if (not_found_handler == nullptr) {
            co_return make_route_not_found_response();
        }
        std::exception_ptr exception;
        try {
            co_return co_await not_found_handler(terminal_context);
        } catch (...) {
            exception = std::current_exception();
        }
        co_return co_await respond_exception(terminal_context, exception);
    };
    std::exception_ptr exception;
    try {
        co_return co_await run_unmatched_chain(context_value, callback_access::bind<task<http_response>(context&)>(terminal));
    } catch (...) {
        exception = std::current_exception();
    }
    co_return co_await finalize_failure(context_value, exception);
}

task<http_response> detail::route_table::respond_exception(context& context_value, std::exception_ptr exception) const {
    return respond_exception(context_value, std::move(exception), error_handler_for(detail::context_access::request(context_value).path()));
}

task<http_response> detail::route_table::respond_exception(context& context_value, std::exception_ptr exception, http_error_handler_ref error_handler) const {
    const auto error_info = classify_exception(exception);
    detail::context_access::set_error(context_value, exception);
    detail::context_access::discard_draft(context_value);
    report_unexpected_failure(error_info, exception, error_handler);

    auto response = co_await invoke_error_handler(context_value, error_info.error_.info(), error_handler);
    apply_exception_response_metadata(response, error_info);
    co_return response;
}

// Which handler answers a failure on a given path: the most specific prefix
// scope that covers it, or the plan-wide fallback.

namespace {

// Deepest-first stored order: the first hit is the tightest scope. Matching
// follows path_is_under_prefix: whole, percent-decoded path segments, so "/api"
// scopes "/api" and "/api/x" but never "/apix".
template <typename stored_type>
[[nodiscard]] auto select_prefix_handler(const std::vector<stored_type>& stored, std::string_view path) noexcept {
    for (const auto& candidate : stored) {
        const std::string_view prefix(candidate.prefix_);
        if (detail::path_is_under_prefix(path, prefix)) {
            return candidate.handler_;
        }
    }
    return decltype(stored.front().handler_){nullptr};
}

}  // namespace

detail::http_error_handler_ref detail::route_table::error_handler_for(std::string_view path) const noexcept {
    const auto handler = select_prefix_handler(plan_->prefix_error_handlers_, path);
    return handler != nullptr ? handler : plan_->error_handler_;
}

detail::http_not_found_handler_ref detail::route_table::not_found_handler_for(std::string_view path) const noexcept {
    const auto handler = select_prefix_handler(plan_->prefix_not_found_handlers_, path);
    return handler != nullptr ? handler : plan_->not_found_handler_;
}

// Running a route that streams its response: binding the writer to the context
// for the handler's lifetime, producing the head the HTTP/1 sink commits, and
// unwinding when the handler throws after bytes are already on the wire.

namespace {

class response_stream_context_binding final {
public:
    explicit response_stream_context_binding(response_stream_writer* writer) noexcept :
        writer_(writer) {}

    ~response_stream_context_binding() {
        if (writer_ != nullptr) {
            detail::streaming_access::release_context(*writer_);
        }
    }

    response_stream_context_binding(const response_stream_context_binding&) = delete;
    response_stream_context_binding& operator=(const response_stream_context_binding&) = delete;

private:
    response_stream_writer* writer_;
};

}  // namespace

task<std::optional<http_response>> detail::route_table::dispatch_stream_route(const http_request& request, const resolved_route& resolved, route_stream_handler handler, const context_services& services, response_stream_writer* response_stream) const {
    const auto& route = resolved.route();
    auto context_value = make_route_context(request, resolved, services);
    auto& context_services_value = detail::context_access::services(context_value);
    context_services_value.bind_routes(*this);
    context_services_value.bind_route_body_limit(route.max_request_body_bytes());
    if (response_stream != nullptr) {
        context_services_value.bind_response_stream(*response_stream);
    }
    const auto* response_stream_output = context_services_value.response_output().response_stream();
    auto* const bound_writer = response_stream_output != nullptr ? &response_stream_output->writer() : nullptr;
    response_stream_context_binding stream_context_binding(bound_writer);
    if (bound_writer != nullptr) {
        detail::streaming_access::bind_context(*bound_writer, context_value, context_value.stop_token());
    }
    co_return co_await dispatch_bound_stream(resolved, context_value, handler);
}

task<std::optional<http_response>> detail::route_table::dispatch_bound_stream(const resolved_route& resolved, context& context_value, route_stream_handler handler) const {
    const auto& route = resolved.route();
    auto& context_services_value = detail::context_access::services(context_value);
    const auto* response_stream_output = context_services_value.response_output().response_stream();
    auto* const bound_writer = response_stream_output != nullptr ? &response_stream_output->writer() : nullptr;

    // Records that next() reached the handler even when it emitted no bytes.
    bool handler_invoked = false;
    std::exception_ptr exception;
    try {
        if (!route.has_middleware()) {
            handler_invoked = true;
            co_await handler(context_value);
        } else {
            // The stream terminal: a websocket or tunnel upgrade commits the
            // session the middleware chain opened before its handshake head,
            // then the handler runs. A response stream commits it explicitly
            // with commit() (or end() for an empty response).
            auto terminal = [&handler_invoked, &handler, &route](context& terminal_context) -> task<void> {
                if (route.endpoint().response_stream() == nullptr && session_access::available(terminal_context)) {
                    co_await session_access::commit(terminal_context);
                }
                handler_invoked = true;
                co_await handler(terminal_context);
            };
            auto chain = route_chain(route);
            chain.terminal_ = callback_access::bind<task<void>(context&)>(terminal);
            co_await run_chain(chain, context_value);
            if (!detail::context_access::has_response(context_value)) {
                if (auto context_exception = context_value.exception()) {
                    std::rethrow_exception(context_exception);
                }
                if (!handler_invoked && !detail::context_access::output_committed(context_value)) {
                    throw std::logic_error(
                        "context is not finalized; stream middleware must select a response, "
                        "commit the stream, or await next()");
                }
            }
        }
    } catch (...) {
        exception = std::current_exception();
    }
    if (context_services_value.connection_aborted() || context_services_value.request_reset()) {
        if (exception != nullptr) {
            std::rethrow_exception(exception);
        }
        if (auto context_exception = context_value.exception()) {
            std::rethrow_exception(context_exception);
        }
        co_return std::nullopt;
    }

    if (exception != nullptr) {
        if (detail::context_access::output_committed(context_value)) {
            std::rethrow_exception(exception);
        }
        auto response = co_await finalize_failure(context_value, exception);
        co_return std::move(response);
    }

    // The middleware chain converts a handler exception into a buffered error
    // response and records it via context.exception() (contain_failure
    // -> respond_exception -> set_error), so a mid-request failure does not surface as
    // a local exception above. Once the output is committed -- a started stream or
    // a websocket/tunnel handshake -- that buffered response can no longer be sent,
    // and finalizing the stream with a clean terminator would frame a truncated
    // body as complete. Rethrow so the driver closes the connection, exactly as
    // the no-middleware path does above. Middleware may
    // still replace an uncommitted response stream, or a websocket terminal that
    // failed during handshake preparation, with a buffered response after `next()`.
    const bool output_committed = detail::context_access::output_committed(context_value);
    if (output_committed) {
        if (auto context_exception = context_value.exception()) {
            std::rethrow_exception(context_exception);
        }
    } else if (detail::context_access::has_response(context_value)) {
        co_return detail::context_access::release_response(context_value);
    }
    if (handler_invoked || output_committed) {
        // The bound context is local to this coroutine. Finish a response stream
        // before it is destroyed so an empty/bodyless handler cannot leave the
        // sink with a dangling context* that a higher layer later dereferences.
        // Reaching this common point gives middleware post-processing its full
        // chance to adjust an uncommitted head; an already committed writer no
        // longer needs the context.
        if (bound_writer != nullptr && (!output_committed || bound_writer->committed())) {
            try {
                co_await bound_writer->end();
            } catch (...) {
                exception = std::current_exception();
            }
            if (exception != nullptr) {
                if (detail::context_access::output_committed(context_value)) {
                    std::rethrow_exception(exception);
                }
                co_return co_await finalize_failure(context_value, exception);
            }
        }
        co_return std::nullopt;
    }
    throw std::logic_error("stream route completed without a response or handled output");
}

}  // namespace ruvia
