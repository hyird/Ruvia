#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include "blocking_pool.h"
#include "borrowed_text.h"
#include "connection_lifecycle.h"
#include "context_access.h"
#include "controller.h"
#include "event_loop.h"
#include "http1_server_response.h"
#include "http_client.h"
#include "http_connect_udp.h"
#include "http_header.h"
#include "http_request.h"
#include "http_server.h"
#include "middleware.h"
#include "route_table.h"
#include "streaming.h"
#include "task_scope.h"
#include "worker_ref.h"

#ifdef RUVIA_ENABLE_DATABASE
#include "db_session.h"
#endif
#ifdef RUVIA_ENABLE_REDIS
#include "redis.h"
#endif

#include <chrono>
#include <compare>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <limits>
#include <random>
#include <type_traits>

#include "context.h"
#include "http_cache.h"
#include "model.h"
#include "stop_token.h"
#include "task.h"
#include "websocket_handshake.h"

namespace ruvia::detail {

enum class route_segment_kind { literal,
    param,
    wildcard };

[[nodiscard]] constexpr route_segment_kind classify_route_segment(std::string_view segment) noexcept {
    if (segment == "*") {
        return route_segment_kind::wildcard;
    }
    return !segment.empty() && segment.front() == ':' ? route_segment_kind::param : route_segment_kind::literal;
}

[[nodiscard]] inline bool is_dot_segment(std::string_view segment) noexcept {
    // The longest spelling is "%2E%2E"; the size check keeps ordinary segments cheap.
    return !segment.empty() && segment.size() <= 6 && (uri_paths_equivalent(segment, ".") || uri_paths_equivalent(segment, ".."));
}

// Build, reverse routing and request matching share the same segments.
// Each remainder retains its leading slash, preserving empty segments without
// allowing them to match a parameter. A final slash yields a trailing empty
// segment, so "/a/" and "/a" are distinct paths (RFC 3986) for dynamic routes
// exactly as they are for byte-exact static routes.
[[nodiscard]] inline bool split_path_segment(std::string_view path, std::string_view& segment, std::string_view& rest) noexcept {
    if (path.empty()) {
        segment = {};
        rest = {};
        return false;
    }
    if (path.front() == '/') {
        path.remove_prefix(1);
    }

    const auto slash = path.find('/');
    if (slash == std::string_view::npos) {
        segment = path;
        rest = {};
        return true;
    }

    segment = path.substr(0, slash);
    rest = path.substr(slash);  // keep the leading '/' so empty segments survive
    return true;
}

// Without parameter names, equality means the same trie slot: literal children
// compare canonically, parameters share one child, and wildcards share one slot.
// Other overlaps have a deterministic literal-before-parameter-before-wildcard
// winner and are not conflicts.
[[nodiscard]] inline bool route_patterns_equivalent(std::string_view left, std::string_view right, bool compare_param_names) noexcept {
    for (;;) {
        std::string_view left_segment;
        std::string_view left_rest;
        std::string_view right_segment;
        std::string_view right_rest;
        const auto has_left = split_path_segment(left, left_segment, left_rest);
        const auto has_right = split_path_segment(right, right_segment, right_rest);
        if (!has_left || !has_right) {
            return has_left == has_right;
        }
        const auto kind = classify_route_segment(left_segment);
        if (kind != classify_route_segment(right_segment) || ((kind == route_segment_kind::literal || compare_param_names) && !uri_paths_equivalent(left_segment, right_segment))) {
            return false;
        }
        left = left_rest;
        right = right_rest;
    }
}

// Whether any segment of `path` is "." or "..", in any percent-encoded
// spelling (RFC 3986 6.2.2.2). Requests carrying one are rejected before
// routing, so a route pattern containing one could never be matched.
[[nodiscard]] inline bool has_dot_segment(std::string_view path) noexcept {
    std::size_t cursor = 0;
    for (;;) {
        const auto slash = path.find('/', cursor);
        const auto end = slash == std::string_view::npos ? path.size() : slash;
        if (is_dot_segment(path.substr(cursor, end - cursor))) {
            return true;
        }
        if (slash == std::string_view::npos) {
            return false;
        }
        cursor = slash + 1;
    }
}

}  // namespace ruvia::detail

namespace ruvia {

using namespace detail;

namespace {

// app_configuration already normalized, null-checked and de-duplicated the prefixes.
template <typename stored_type, typename registration_type>
void replace_prefix_handlers(std::vector<stored_type>& stored, std::span<const registration_type> handlers) {
    std::vector<stored_type> ordered;
    ordered.reserve(handlers.size());
    for (const auto& registration : handlers) {
        ordered.emplace_back(std::string_view(registration.first), callback_access::ref(registration.second));
    }
    // Deepest prefix first: selection is a first-match scan. A nested scope
    // always has more segments; distinct scopes of equal depth cannot nest, so
    // their relative order is irrelevant; keep it stable.
    std::ranges::stable_sort(ordered, [](const stored_type& left, const stored_type& right) noexcept {
        return detail::fallback_prefix_depth(left.prefix_) > detail::fallback_prefix_depth(right.prefix_);
    });
    stored = std::move(ordered);
}

}  // namespace

detail::router::router() :
    plan_(std::make_unique<compiled_route_plan>()),
    route_hashes_(),
    route_middleware_descriptors_(),
    route_middleware_ranges_(),
    global_middleware_descriptors_() {}

detail::router::~router() = default;

void detail::router::set_error_handler(http_error_handler_ref handler) noexcept {
    plan_->error_handler_ = handler;
}

void detail::router::set_not_found_handler(http_not_found_handler_ref handler) noexcept {
    plan_->not_found_handler_ = handler;
}

void detail::router::set_prefix_error_handlers(std::span<const std::pair<std::string, callback_registration<http_error_handler>>> handlers) {
    replace_prefix_handlers(plan_->prefix_error_handlers_, handlers);
}

void detail::router::set_prefix_not_found_handlers(std::span<const std::pair<std::string, callback_registration<http_not_found_handler>>> handlers) {
    replace_prefix_handlers(plan_->prefix_not_found_handlers_, handlers);
}

void detail::router::set_global_middlewares(std::span<const controller_middleware_descriptor> descriptors) {
    for (const auto& descriptor : descriptors) {
        plan_->has_route_rate_limit_ = plan_->has_route_rate_limit_ || descriptor.uses_route_rate_limit();
    }
    global_middleware_descriptors_.assign(descriptors.begin(), descriptors.end());
}

std::size_t detail::router::append_route_middlewares(std::span<const controller_middleware_descriptor> controller_middlewares, std::span<const controller_middleware_descriptor> route_middlewares) {
    const auto offset = route_middleware_descriptors_.size();
    std::size_t body_limit = 0;
    for (const auto middlewares : {controller_middlewares, route_middlewares}) {
        for (const auto& descriptor : middlewares) {
            const auto declared_limit = descriptor.request_body_limit();
            if (declared_limit != 0) {
                if (body_limit == 0 || declared_limit < body_limit) {
                    body_limit = declared_limit;
                }
                continue;
            }
            // Declarations the server reads before the body is accepted.
            if (descriptor.stream_request_body()) {
                continue;
            }
            const auto* const key = descriptor.validated_model_type_key();
            if (key != nullptr && std::ranges::any_of(std::span(route_middleware_descriptors_).subspan(offset), [key](const auto& registered) noexcept {
                return registered.validated_model_type_key() == key;
            })) {
                throw std::invalid_argument("duplicate validated model type on route");
            }
            plan_->has_route_rate_limit_ = plan_->has_route_rate_limit_ || descriptor.uses_route_rate_limit();
            route_middleware_descriptors_.push_back(descriptor);
        }
    }
    route_middleware_ranges_.push_back({offset, route_middleware_descriptors_.size() - offset});
    return body_limit;
}

void detail::router::declare_controllers(std::span<const controller_registrar> registrars, const controller_store& controllers) {
    const auto initial_route_capacity = registrars.size() * 2U;
    plan_->routes_.reserve(initial_route_capacity);
    route_hashes_.reserve(initial_route_capacity);
    route_middleware_ranges_.reserve(initial_route_capacity);
    for (std::size_t slot = 0; slot < registrars.size(); ++slot) {
        controller_slot_ = slot;
        controller_target_ = controllers.instance(slot);
        registrars[slot].register_routes_(controller_target_, *this);
    }
    controller_target_ = nullptr;
}

void detail::router::validate_route_target(http_known_method method, std::string_view method_token, std::string_view path, const route_endpoint& endpoint) const {
    if (const auto* tunnel = endpoint.tunnel()) {
        if (method != http_known_method::connect || !method_token.empty()) {
            throw std::invalid_argument("tunnel route requires CONNECT");
        }
        if (tunnel->protocol().empty()) {
            if (path != "*" && !is_valid_http_connect_authority(path)) {
                throw std::invalid_argument("CONNECT route requires host:port authority or *");
            }
        } else if ((path.find('?') != std::string_view::npos) || !is_valid_http_origin_form_target(path) || has_dot_segment(path)) {
            throw std::invalid_argument("upgrade-protocol tunnel route requires an origin-form path");
        }
        return;
    }
    // An extension route carries unknown plus a token; anything else must sit
    // in the enum-indexed fast path.
    if (method_token.empty() && !route_table::is_routable_method(method)) {
        throw std::invalid_argument("route method must be routable");
    }
    // Only built-in methods have dynamic-segment tries; extension methods
    // use the immutable static-path index.
    if (!method_token.empty() && is_dynamic_path(path)) {
        throw std::invalid_argument("extension method routes must use a static path");
    }
    if ((path.find('?') != std::string_view::npos) || !ruvia::is_valid_http_origin_form_target(path)) {
        throw std::invalid_argument("route path must be an origin-form path without query");
    }
    // resolve() rejects every request carrying a dot segment before lookup,
    // so such a pattern would register a handler nothing can reach.
    if (has_dot_segment(path)) {
        throw std::invalid_argument("route path must not contain dot segments");
    }
}

detail::compiled_route_plan_ptr_type detail::router::compile() {
    validate_no_dynamic_route_conflict();
    build_middleware_frames();
    auto& plan = *plan_;
    for (const auto& route : plan.routes_) {
        if (route.endpoint().tunnel() != nullptr) {
            plan.connect_route_indices_.push_back(route.index());
        }
        if (route.method() == http_known_method::unknown) {
            plan.extension_route_indices_.push_back(route.index());
        }
    }
    build_extension_index();
    build_static_method_mask();
    build_static_index();
    build_dynamic_routes();
    build_url_patterns();
    return std::move(plan_);
}

detail::compiled_route_plan_ptr_type detail::compile_route_plan(std::span<const controller_registrar> registrars, const controller_store& controllers, const app_configuration& configuration) {
    router routes_value;
    routes_value.declare_controllers(registrars, controllers);
    configuration.apply(routes_value);
    return routes_value.compile();
}

void detail::router::register_endpoint(http_known_method method, std::string_view method_token, std::string_view path, route_endpoint endpoint, void* handler_target, std::span<const controller_middleware_descriptor> controller_middlewares, std::span<const controller_middleware_descriptor> route_middlewares) {
    if (controller_target_ == nullptr) {
        throw std::logic_error("routes are declared only while compiling the route plan");
    }
    if (handler_target != controller_target_) {
        throw std::logic_error("route handler must be bound to the controller declaring it");
    }
    register_owned_endpoint(controller_slot_, method, method_token, path, std::move(endpoint), controller_middlewares, route_middlewares);
}

void detail::router::register_owned_endpoint(std::size_t slot, http_known_method method, std::string_view method_token, std::string_view path, route_endpoint endpoint, std::span<const controller_middleware_descriptor> controller_middlewares, std::span<const controller_middleware_descriptor> route_middlewares) {
    if (!method_token.empty()) {
        if (!is_valid_http_method_token(method_token)) {
            throw std::invalid_argument("extension route method must be a valid HTTP method token");
        }
        if (classify_http_method(method_token) != http_known_method::unknown) {
            throw std::invalid_argument("extension route method is a known method; register it with its typed route macro");
        }
    }
    validate_route_target(method, method_token, path, endpoint);

    auto& routes = plan_->routes_;
    const auto* tunnel = endpoint.tunnel();
    auto hash = route_table::route_hash(method, route_table::path_hash(tunnel == nullptr ? std::string_view(path) : tunnel->protocol()));
    if (!method_token.empty()) {
        hash ^= route_table::path_hash(method_token);
    }
    for (std::size_t i = 0; i < route_hashes_.size(); ++i) {
        if (route_hashes_[i] != hash) {
            continue;
        }
        const auto& registered = routes[i];
        if (registered.method() != method || registered.method_token() != method_token) {
            continue;
        }
        if (tunnel != nullptr) {
            const auto* registered_tunnel = registered.endpoint().tunnel();
            if (registered_tunnel != nullptr && registered_tunnel->protocol() == tunnel->protocol() && (tunnel->protocol().empty() ? registered.path() == path || http_authorities_equal(path, registered.path(), 0) : route_patterns_equivalent(registered.path(), path, /*compare_param_names=*/true))) {
                throw std::invalid_argument("duplicate CONNECT route registration");
            }
        } else if (route_patterns_equivalent(registered.path(), path, /*compare_param_names=*/true)) {
            throw std::invalid_argument("duplicate route registration");
        }
    }

    const bool dynamic = (tunnel == nullptr || !tunnel->protocol().empty()) && is_dynamic_path(path);
    const auto index = routes.size();
    const auto max_request_body_bytes = append_route_middlewares(controller_middlewares, route_middlewares);
    routes.emplace_back(route_entry::init_type{.index_ = index, .controller_ = slot, .method_ = method, .method_token_ = method_token, .path_ = path, .endpoint_ = std::move(endpoint), .dynamic_ = dynamic, .max_request_body_bytes_ = max_request_body_bytes});
    route_hashes_.push_back(hash);
}

void detail::router::build_static_index() {
    auto& plan = *plan_;
    const auto is_indexed = [](const route_entry& route) {
        return !route.dynamic() && route_table::is_routable_method(route.method());
    };
    const auto route_count = static_cast<std::size_t>(std::ranges::count_if(plan.routes_, is_indexed));
    if (route_count == 0) {
        return;
    }
    if (route_count > plan.static_slots_.max_size() / 2) {
        throw std::length_error("too many static routes");
    }

    plan.static_slots_.resize(std::bit_ceil(route_count * 2));
    plan.static_slot_mask_ = plan.static_slots_.size() - 1;
    for (const auto& route : plan.routes_) {
        if (!is_indexed(route)) {
            continue;
        }
        const auto hash = route_table::route_hash(route.method(), route_table::path_hash(route.path()));
        auto slot_index = static_cast<std::size_t>(hash) & plan.static_slot_mask_;
        while (plan.static_slots_[slot_index].route_index_ != compiled_route_plan::no_route_index) {
            slot_index = (slot_index + 1) & plan.static_slot_mask_;
        }
        plan.static_slots_[slot_index] = {hash, route.index()};
    }
}

void detail::router::build_static_method_mask() {
    auto& plan = *plan_;
    for (const auto& route : plan.routes_) {
        if (route_table::is_routable_method(route.method()) && !route.dynamic()) {
            plan.static_method_mask_ |= 1U << route_table::method_index(route.method());
        }
    }
}

void detail::router::build_extension_index() {
    auto& plan = *plan_;
    auto& indices = plan.extension_route_indices_;
    std::ranges::sort(indices, [&plan](std::size_t left, std::size_t right) noexcept {
        const auto order = compare_uri_paths(plan.routes_[left].path(), plan.routes_[right].path());
        return order != 0 ? order < 0 : left < right;
    });
    plan.extension_methods_.reserve(indices.size());
    for (std::size_t first = 0; first < indices.size();) {
        const auto path = plan.routes_[indices[first]].path();
        const auto offset = plan.extension_methods_.size();
        auto last = first;
        do {
            plan.extension_methods_.push_back(plan.routes_[indices[last]].method_token());
            ++last;
        } while (last < indices.size() && uri_paths_equivalent(plan.routes_[indices[last]].path(), path));
        plan.extension_paths_.push_back({.path_ = path, .methods_ = std::span<const std::string_view>(plan.extension_methods_).subspan(offset)});
        first = last;
    }
    std::ranges::sort(indices, [&plan](std::size_t left, std::size_t right) noexcept {
        const auto& left_route = plan.routes_[left];
        const auto& right_route = plan.routes_[right];
        if (left_route.method_token() != right_route.method_token()) {
            return left_route.method_token() < right_route.method_token();
        }
        return compare_uri_paths(left_route.path(), right_route.path()) < 0;
    });
}

namespace {

// How much of a route's request-path space one path-scoped middleware covers.
enum class middleware_scope_coverage : std::uint8_t {
    // No request the route can match is under the scope: the frame is omitted.
    none,
    // Some matched requests are under the scope and some are not: the frame is
    // kept with its scope and checked against the request path at dispatch.
    partial,
    // Every request the route can match is under the scope: the frame is
    // unconditional.
    full,
};

// Classifies a route pattern against a normalized scope prefix with the
// matcher's own segment rules, so the answer holds for the request paths the
// route actually serves rather than for the pattern text. A request is under
// the scope exactly when its leading segments are equivalent to the prefix's
// segments (path_is_under_prefix); a literal route segment matches exactly the
// request segments of the same canonical form (uri_paths_equivalent), so
// comparing it with that equivalence is exact.
[[nodiscard]] middleware_scope_coverage route_scope_coverage(std::string_view pattern, bool dynamic, std::string_view prefix) noexcept {
    if (!dynamic) {
        // A static route serves exactly its own path.
        return detail::path_is_under_prefix(pattern, prefix) ? middleware_scope_coverage::full : middleware_scope_coverage::none;
    }
    if (prefix.empty()) {
        return middleware_scope_coverage::full;
    }

    // Set once a parameter faces a prefix segment: it may or may not capture
    // that exact text, so only the request can decide.
    bool request_dependent = false;
    for (;;) {
        std::string_view prefix_segment;
        std::string_view prefix_rest;
        if (!detail::split_path_segment(prefix, prefix_segment, prefix_rest)) {
            return request_dependent ? middleware_scope_coverage::partial : middleware_scope_coverage::full;
        }
        std::string_view pattern_segment;
        std::string_view pattern_rest;
        if (!detail::split_path_segment(pattern, pattern_segment, pattern_rest)) {
            // Every matched request ends above the scope.
            return middleware_scope_coverage::none;
        }
        const auto kind = detail::classify_route_segment(pattern_segment);
        if (kind == detail::route_segment_kind::wildcard) {
            // The capture may be empty or may spell out the rest of the prefix.
            return middleware_scope_coverage::partial;
        }
        if (kind == detail::route_segment_kind::param) {
            if (prefix_segment.empty()) {
                // A parameter never matches an empty segment.
                return middleware_scope_coverage::none;
            }
            request_dependent = true;
        } else if (!uri_paths_equivalent(pattern_segment, prefix_segment)) {
            return middleware_scope_coverage::none;
        }
        prefix = prefix_rest;
        pattern = pattern_rest;
    }
}

[[nodiscard]] std::string describe_route(http_known_method method, std::string_view path) {
    std::string text(known_http_method_token(method));
    text.push_back(' ');
    text.append(path);
    return text;
}

}  // namespace

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

void detail::router::validate_no_dynamic_route_conflict() const {
    const auto& routes = plan_->routes_;
    for (std::size_t i = 0; i < routes.size(); ++i) {
        const auto& left = routes[i];
        if (!left.dynamic()) {
            continue;
        }
        const auto* left_tunnel = left.endpoint().tunnel();
        for (std::size_t j = i + 1; j < routes.size(); ++j) {
            const auto& right = routes[j];
            if (!right.dynamic() || left.method() != right.method()) {
                continue;
            }
            // Upgrade-protocol tunnel routes (connect-udp) share one lookup trie
            // per protocol; every other dynamic route shares one per method.
            // Routes in different tries never compete for a request.
            const auto* right_tunnel = right.endpoint().tunnel();
            if ((left_tunnel == nullptr) != (right_tunnel == nullptr) || (left_tunnel != nullptr && left_tunnel->protocol() != right_tunnel->protocol())) {
                continue;
            }
            if (route_patterns_equivalent(left.path(), right.path(), /*compare_param_names=*/false)) {
                throw std::invalid_argument(("conflicting dynamic routes match the same requests with equal precedence: " + describe_route(left.method(), left.path()) + " and " + describe_route(right.method(), right.path())).c_str());
            }
        }
    }
}

void detail::router::build_middleware_frames() {
    auto& plan = *plan_;
    // Instances: the app-wide middlewares first (one each, shared by every
    // route of a worker), then every route's own middlewares in registration
    // order (one instance per route registration).
    const auto global_count = global_middleware_descriptors_.size();
    plan.middleware_instances_.reserve(global_count + route_middleware_descriptors_.size());
    plan.middleware_instances_.assign(global_middleware_descriptors_.begin(), global_middleware_descriptors_.end());
    plan.middleware_instances_.insert(plan.middleware_instances_.end(), route_middleware_descriptors_.begin(), route_middleware_descriptors_.end());

    const auto frame_capacity = plan.routes_.size() * global_count + route_middleware_descriptors_.size() + global_count;
    plan.middleware_frame_instances_.reserve(frame_capacity);
    plan.middleware_scopes_.reserve(frame_capacity);
    for (auto& route : plan.routes_) {
        // App-wide middleware runs before controller/route middleware on every
        // matched route: each route's contiguous frame range starts with the
        // shared global instances.
        //
        // A path-scoped registration (use_at) applies to the request paths
        // under its prefix, decided here once per route against every path the
        // route can match: a route entirely inside the scope gets an
        // unconditional frame, a route entirely outside gets none, and only a
        // route that serves paths on both sides (a parameter or wildcard facing
        // the prefix) keeps the frame with its scope for a dispatch-time check.
        const auto offset = plan.middleware_frame_instances_.size();
        bool conditional_middleware = false;
        for (std::size_t i = 0; i < global_count; ++i) {
            const auto scope = global_middleware_descriptors_[i].prefix();
            const auto coverage = route_scope_coverage(route.path(), route.dynamic(), scope);
            if (coverage == middleware_scope_coverage::none) {
                continue;
            }
            const auto conditional = coverage == middleware_scope_coverage::partial;
            conditional_middleware = conditional_middleware || conditional;
            plan.middleware_frame_instances_.push_back(i);
            plan.middleware_scopes_.push_back(conditional ? scope : std::string_view{});
        }
        const auto range = route_middleware_ranges_[route.index()];
        for (std::size_t i = 0; i < range.count_; ++i) {
            plan.middleware_frame_instances_.push_back(global_count + range.offset_ + i);
            plan.middleware_scopes_.emplace_back();
        }
        route.middleware_offset_ = offset;
        route.middleware_count_ = plan.middleware_frame_instances_.size() - offset;
        route.conditional_middleware_ = conditional_middleware;
    }

    // The unmatched-request block, appended once after every route's range so
    // it stays contiguous and no route can accidentally include it.
    plan.unmatched_middleware_offset_ = plan.middleware_frame_instances_.size();
    for (std::size_t i = 0; i < global_count; ++i) {
        if (global_middleware_descriptors_[i].runs_on_unmatched_requests()) {
            plan.middleware_frame_instances_.push_back(i);
            plan.middleware_scopes_.emplace_back();
        }
    }
    plan.unmatched_middleware_count_ = plan.middleware_frame_instances_.size() - plan.unmatched_middleware_offset_;
}

void detail::router::build_url_patterns() {
    auto& patterns = plan_->url_patterns_;
    patterns.reserve(plan_->routes_.size());
    for (const auto& route : plan_->routes_) {
        const auto* tunnel = route.endpoint().tunnel();
        if (tunnel != nullptr && tunnel->protocol().empty()) {
            continue;
        }
        patterns.push_back(route.path());
    }
    std::ranges::sort(patterns);
    const auto duplicates = std::ranges::unique(patterns);
    patterns.erase(duplicates.begin(), duplicates.end());
}

namespace {

struct dynamic_route_upper_bounds final {
    std::size_t nodes_{0};
    std::size_t param_names_{0};
};

[[nodiscard]] dynamic_route_upper_bounds dynamic_upper_bounds(std::string_view path) noexcept {
    dynamic_route_upper_bounds bounds;
    while (true) {
        std::string_view segment;
        std::string_view rest;
        if (!detail::split_path_segment(path, segment, rest)) {
            return bounds;
        }
        const auto kind = detail::classify_route_segment(segment);
        if (kind == detail::route_segment_kind::wildcard) {
            ++bounds.param_names_;
        } else {
            ++bounds.nodes_;
            if (kind == detail::route_segment_kind::param) {
                ++bounds.param_names_;
            }
        }
        if (rest.empty()) {
            return bounds;
        }
        path = rest;
    }
}

// Templated so the plan's private node type is never named outside its friends.
// Literal children sort by canonical form, the order find_dynamic_static_child
// searches in.
template <typename node_type>
void sort_dynamic_node(node_type& node_value) {
    std::ranges::sort(node_value.static_children_, [](const auto& left, const auto& right) {
        return compare_uri_paths(left.segment_, right.segment_) < 0;
    });
    for (auto& child : node_value.static_children_) {
        sort_dynamic_node(*child.node_);
    }
    if (node_value.param_child_) {
        sort_dynamic_node(*node_value.param_child_);
    }
}

}  // namespace

void detail::router::build_dynamic_routes() {
    auto& plan = *plan_;
    plan.connect_protocols_.reserve(plan.connect_route_indices_.size());

    std::size_t dynamic_node_capacity = 0;
    std::size_t param_name_capacity = 0;
    for (const auto& route : plan.routes_) {
        if (route.dynamic()) {
            const auto bounds = dynamic_upper_bounds(route.path());
            dynamic_node_capacity += bounds.nodes_;
            param_name_capacity += bounds.param_names_;
        }
    }
    // Both reserved up front: trie nodes and parameter-name views are borrowed
    // by address, so neither vector may reallocate while it is filled.
    plan.dynamic_node_arena_.reserve(dynamic_node_capacity);
    plan.param_names_.reserve(param_name_capacity);

    for (auto& route : plan.routes_) {
        if (!route.dynamic()) {
            continue;
        }
        if (const auto* tunnel = route.endpoint().tunnel()) {
            const auto protocol = tunnel->protocol();
            auto found = std::ranges::find(plan.connect_protocols_, protocol, &compiled_route_plan::connect_protocol_index_type::protocol_);
            if (found == plan.connect_protocols_.end()) {
                found = plan.connect_protocols_.emplace(plan.connect_protocols_.end(), protocol);
            }
            insert_dynamic(found->root_, route);
        } else {
            const auto method = route_table::method_index(route.method());
            plan.dynamic_method_mask_ |= 1U << method;
            insert_dynamic(plan.dynamic_roots_[method], route);
        }
    }
    for (auto& protocol : plan.connect_protocols_) {
        sort_dynamic_node(protocol.root_);
    }
    for (auto& root : plan.dynamic_roots_) {
        sort_dynamic_node(root);
    }
}

bool detail::router::is_dynamic_path(std::string_view path) noexcept {
    while (!path.empty()) {
        std::string_view segment;
        std::string_view rest;
        if (!split_path_segment(path, segment, rest)) {
            return false;
        }
        if (classify_route_segment(segment) != route_segment_kind::literal) {
            return true;
        }
        path = rest;
    }

    return false;
}

void detail::router::append_dynamic_param_name(route_entry& route, std::string_view name) {
    auto& names_storage = plan_->param_names_;
    const auto names = route.param_names();
    if (names.size() >= max_route_params) {
        throw std::invalid_argument("route has too many parameters");
    }

    const auto offset = names.empty() ? names_storage.size() : static_cast<std::size_t>(names.data() - names_storage.data());
    names_storage.push_back(name);
    route.param_names_ = std::span<const std::string_view>(names_storage.data() + offset, names.size() + 1);
}

void detail::router::insert_dynamic(compiled_route_plan::dynamic_node_type& root, route_entry& route) {
    auto path = route.path();
    auto* node_value = &root;
    auto& plan = *plan_;

    while (true) {
        std::string_view segment;
        std::string_view rest;
        if (!split_path_segment(path, segment, rest)) {
            node_value->route_index_ = route.index();
            return;
        }
        const auto kind = classify_route_segment(segment);
        if (kind == route_segment_kind::wildcard) {
            if (!rest.empty()) {
                throw std::invalid_argument("wildcard route segment must be final");
            }
            append_dynamic_param_name(route, "*");
            node_value->wildcard_route_index_ = route.index();
            return;
        }
        if (kind == route_segment_kind::param) {
            if (segment.size() == 1) {
                throw std::invalid_argument("route parameter name must not be empty");
            }
            append_dynamic_param_name(route, segment.substr(1));
            if (!node_value->param_child_) {
                plan.dynamic_node_arena_.emplace_back();
                node_value->param_child_ = &plan.dynamic_node_arena_.back();
            }
            node_value = node_value->param_child_;
        } else {
            compiled_route_plan::dynamic_node_type* child_node = nullptr;
            for (auto& child_value : node_value->static_children_) {
                if (uri_paths_equivalent(child_value.segment_, segment)) {
                    child_node = child_value.node_;
                    break;
                }
            }
            if (child_node == nullptr) {
                plan.dynamic_node_arena_.emplace_back();
                child_node = &plan.dynamic_node_arena_.back();
                node_value->static_children_.push_back(compiled_route_plan::dynamic_static_child_type{segment, child_node});
            }
            node_value = child_node;
        }

        if (rest.empty()) {
            node_value->route_index_ = route.index();
            return;
        }
        path = rest;
    }
}

// Substitute parameters and percent-encode path values while preserving the
// same segments used to build and resolve the registered route.

namespace {

// RFC 3986 pchar minus pct-encoded: bytes a path segment may carry verbatim.
[[nodiscard]] constexpr bool is_url_for_segment_byte(char value) noexcept {
    return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9') || value == '-' || value == '.' || value == '_' || value == '~' || value == '!' || value == '$' || value == '&' || value == '\'' || value == '(' || value == ')' || value == '*' || value == '+' || value == ',' || value == ';' || value == '=' || value == ':' || value == '@';
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
    const auto path = detail::decode_request_path(request.path(), decoded.emplace());
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
            // One handle_error co_await serves both rejection kinds: each
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
                response = co_await handle_error(request, *error, services);
                if (allowed_methods != 0 || !extension_methods.empty()) {
                    response.allow_methods(allowed_methods, extension_methods);
                }
                co_return;
            }

            response = co_await handle_not_found(request, services);
            co_return;
        }

        (void)co_await dispatch_resolved_route(request, *resolved, services, nullptr, response);
        co_return;
    } catch (...) {
        dispatch_exception = std::current_exception();
    }
    response = co_await handle_exception(request, dispatch_exception, services);
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
        response = co_await handle_exception(request, dispatch_exception, services);
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

task<http_response> detail::route_table::handle_error(const http_request& request, http_error_info error, const context_services& services) const {
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

task<http_response> detail::route_table::handle_exception(const http_request& request, std::exception_ptr exception, const context_services& services) const {
    return handle_exception(request, std::move(exception), services, services.request_stream() ? nullptr : error_handler_for(request.path()));
}

task<http_response> detail::route_table::handle_exception(const http_request& request, std::exception_ptr exception, const context_services& services, http_error_handler_ref error_handler) const {
    if (error_handler == nullptr) {
        auto answer = make_default_exception_response(exception);
        if (answer.unexpected_) {
            ruvia::report_unhandled_failure("request handler", exception);
        }
        co_return std::move(answer.response_);
    }

    auto context_value = detail::context_access::make(request, services);
    detail::context_access::services(context_value).bind_routes(*this);
    auto response = co_await handle_exception(context_value, exception, error_handler);
    detail::context_access::apply_response_policy(context_value, response);
    co_return response;
}

task<http_response> detail::route_table::handle_not_found(const http_request& request, const context_services& services) const {
    const auto not_found_handler = not_found_handler_for(request.path());
    if (not_found_handler == nullptr && plan_->unmatched_middleware_count_ == 0) {
        co_return make_route_not_found_response();
    }

    auto context_value = detail::context_access::make(request, services);
    detail::context_access::services(context_value).bind_routes(*this);
    // The handler's own failure keeps going through handle_exception, exactly as
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
        co_return co_await handle_exception(terminal_context, exception);
    };
    std::exception_ptr exception;
    try {
        co_return co_await run_unmatched_chain(context_value, callback_access::bind<task<http_response>(context&)>(terminal));
    } catch (...) {
        exception = std::current_exception();
    }
    co_return co_await finalize_failure(context_value, exception);
}

task<http_response> detail::route_table::handle_exception(context& context_value, std::exception_ptr exception) const {
    return handle_exception(context_value, std::move(exception), error_handler_for(detail::context_access::request(context_value).path()));
}

task<http_response> detail::route_table::handle_exception(context& context_value, std::exception_ptr exception, http_error_handler_ref error_handler) const {
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

namespace detail {

struct next_access final {
    [[nodiscard]] static next& make_at(void* storage, next_state state_value, next_invoke_type invoke) noexcept {
        return *::new (storage) next(state_value, invoke);
    }
};

}  // namespace detail

namespace {

void store_next_misuse_error(context& context_value, const char* message, std::string_view code) {
    detail::context_access::set_error(context_value, std::make_exception_ptr(std::logic_error(message)));
    detail::context_access::set_response(context_value, detail::make_default_error_response(http_error_info({.status_ = ruvia::http_status::internal_server_error, .code_ = code, .message_ = message})));
}

[[nodiscard]] bool validate_next_invocation(const detail::next_state& state_value) {
    auto& context_value = *state_value.context_;
    if (state_value.invocation_ != detail::next_state::invocation_type::ready) {
        store_next_misuse_error(context_value, "next() called multiple times", "next_called_multiple_times");
        return false;
    }
    if (detail::context_access::has_response(context_value)) {
        store_next_misuse_error(context_value, "next() called after respond()", "next_called_after_response");
        return false;
    }
    return true;
}

task<void> ignore_expired_next(detail::next_state) {
    co_return;
}

// Expires a hop's next() when the frame that received it completes, so a
// stored next() called later is ignored.
class next_control_scope final {
public:
    explicit next_control_scope(detail::next_state::control_type& control) noexcept :
        control_(&control) {}

    next_control_scope(const next_control_scope&) = delete;
    next_control_scope& operator=(const next_control_scope&) = delete;

    ~next_control_scope() {
        control_->expire();
    }

private:
    detail::next_state::control_type* control_;
};

// One hop's control block and the next() handed to its frame share a single
// request-arena allocation; both outlive the frame so a late next() stays safe.
struct next_hop_layout final {
    static constexpr std::size_t next_offset = (sizeof(detail::next_state::control_type) + alignof(next) - 1) / alignof(next) * alignof(next);
    static constexpr std::size_t size = next_offset + sizeof(next);
    static constexpr std::size_t alignment = alignof(next) > alignof(detail::next_state::control_type) ? alignof(next) : alignof(detail::next_state::control_type);
};

task<void> run_chain_hop(detail::next_state state_value);

[[nodiscard]] std::size_t applicable_frame(const detail::middleware_chain& chain, std::size_t index, const context& context_value) noexcept {
    if (chain.scopes_.empty()) {
        return index;
    }
    // The same path the route table resolved this request against.
    const auto path = detail::context_access::request(context_value).path();
    while (index < chain.frames_.size() && !detail::path_is_under_prefix(path, chain.scopes_[index])) {
        ++index;
    }
    return index;
}

// One coroutine per hop: the entry hop (no control) lets failures propagate to
// the dispatcher; every hop reached through next() validates the single-shot
// invocation first and contains failures below it through the chain's policy.
task<void> run_chain_hop(detail::next_state state_value) {
    auto& context_value = *state_value.context_;
    const auto& chain = *state_value.chain_;
    const bool contained = state_value.control_ != nullptr;
    if (contained && !validate_next_invocation(state_value)) {
        co_return;
    }
    std::exception_ptr exception;
    try {
        const auto index = applicable_frame(chain, state_value.index_, context_value);
        if (index >= chain.frames_.size()) {
            co_await chain.terminal_(context_value);
        } else {
            auto* storage = static_cast<std::byte*>(detail::context_access::allocate_request_storage(context_value, next_hop_layout::size, next_hop_layout::alignment));
            auto& control = *std::construct_at(reinterpret_cast<detail::next_state::control_type*>(storage));
            const next_control_scope control_scope(control);
            auto& next_value = detail::next_access::make_at(storage + next_hop_layout::next_offset, detail::next_state{.chain_ = &chain, .context_ = &context_value, .control_ = &control, .index_ = index + 1}, &run_chain_hop);
            co_await chain.frames_[index](context_value, next_value);
        }
    } catch (...) {
        if (!contained) {
            throw;
        }
        exception = std::current_exception();
    }
    if (exception != nullptr) {
        co_await chain.failure_(*chain.table_, context_value, exception);
    }
}

}  // namespace

next::awaitable next::operator()() & {
    auto state_value = state_;
    auto* control = state_value.control_;
    state_value.invocation_ = control == nullptr ? detail::next_state::invocation_type::expired : control->begin_invocation();
    if (state_value.invocation_ == detail::next_state::invocation_type::expired) {
        return awaitable(state_value, &ignore_expired_next);
    }
    return awaitable(state_value, invoke_);
}

task<void> detail::run_chain(const middleware_chain& chain, context& context_value) {
    return run_chain_hop(next_state{.chain_ = &chain, .context_ = &context_value});
}

detail::middleware_chain detail::route_table::route_chain(const route_entry& route) const noexcept {
    const auto offset = route.middleware_offset();
    const auto count = route.middleware_count();
    return middleware_chain{.table_ = this, .frames_ = std::span<const route_middleware>(middleware_frames_).subspan(offset, count), .scopes_ = route.has_conditional_middleware() ? std::span<const std::string_view>(plan_->middleware_scopes_).subspan(offset, count) : std::span<const std::string_view>{}, .terminal_ = {}, .failure_ = &route_table::contain_failure};
}

namespace {

task<http_response> mark_explicit_head_route_response(task<http_response> operation) {
    auto response = co_await std::move(operation);
    if (!response.has_file_representation() && response.body_bytes().empty() && !response.status().is_redirection() && !response.status().is_error()) {
        response.head_metadata_body();
    }
    co_return response;
}

}  // namespace

task<http_response> detail::route_table::invoke_route(const resolved_route& resolved, context& context_value) const {
    const auto& route = resolved.route();
    if (route.endpoint().buffered() == nullptr) {
        throw std::logic_error("route is not a buffered-response route");
    }
    // Hot path: a route with no middleware goes straight to the handler. The
    // context response slot is only needed when middleware can observe the
    // downstream response through context::response().
    auto operation = !route.has_middleware() ? resolved.handler()(context_value) : respond_through_chain(route_chain(route), context_value, resolved.handler());
    if (route.method() == http_known_method::head) {
        return mark_explicit_head_route_response(std::move(operation));
    }
    return operation;
}

task<http_response> detail::route_table::respond_through_chain(middleware_chain chain, context& context_value, route_handler terminal) const {
    auto respond = [terminal](context& terminal_context) -> task<void> {
        detail::context_access::set_response(terminal_context, co_await terminal(terminal_context));
    };
    chain.terminal_ = callback_access::bind<task<void>(context&)>(respond);
    co_await run_chain(chain, context_value);
    if (detail::context_access::has_response(context_value)) {
        co_return detail::context_access::release_response(context_value);
    }
    if (auto exception = context_value.exception()) {
        co_return co_await finalize_failure(context_value, exception);
    }
    throw std::logic_error("context is not finalized; middleware must set a response or await next()");
}

task<http_response> detail::route_table::run_unmatched_chain(context& context_value, route_handler terminal) const {
    return respond_through_chain(middleware_chain{.table_ = this, .frames_ = std::span<const route_middleware>(middleware_frames_).subspan(plan_->unmatched_middleware_offset_, plan_->unmatched_middleware_count_), .scopes_ = {}, .terminal_ = {}, .failure_ = &route_table::contain_failure}, context_value, terminal);
}

task<void> detail::route_table::contain_failure(const route_table& table, context& context_value, std::exception_ptr exception) {
    if (detail::context_access::output_committed(context_value)) {
        detail::context_access::set_error(context_value, std::move(exception));
        co_return;
    }
    auto response = co_await table.handle_exception(context_value, exception);
    detail::context_access::set_response(context_value, std::move(response));
}

task<http_response> detail::route_table::finalize_failure(context& context_value, std::exception_ptr exception) const {
    co_await contain_failure(*this, context_value, std::move(exception));
    co_return detail::context_access::release_response(context_value);
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
    // -> handle_exception -> set_error), so a mid-request failure does not surface as
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

namespace ruvia::detail {
namespace {

// The transport still owns framing, timeouts and the stream limit. This view
// can only tighten its wire-byte budget and buffers lazily, never from a peer's
// advertised length. Storage counts against the same inbound budget.
class routed_body final {
public:
    routed_body(body_reader* source, std::optional<std::size_t> limit, inbound_buffer_account* inbound) :
        source_(source),
        limit_(limit),
        reservation_(inbound) {}

    task<std::optional<std::span<const std::byte>>> read(operation_options operation = {}) {
        if (failure_) {
            std::rethrow_exception(failure_);
        }
        // An Upgrade/CONNECT request has no HTTP content; subsequent bytes
        // belong exclusively to the transport's tunnel acceptance operation.
        if (source_ == nullptr) {
            co_return std::nullopt;
        }
        const operation_steps steps(operation);
        try {
            auto chunk = co_await source_->read(std::move(operation));
            if (chunk && limit_) {
                if (chunk->size() > *limit_ - consumed_) {
                    throw http_request_body_protocol_error(http_request_body_failure::too_large);
                }
                consumed_ += chunk->size();
            }
            co_return chunk;
        } catch (const std::system_error& error) {
            // An interrupted read leaves the underlying stream readable.
            if (!steps.interrupted(error)) {
                failure_ = std::current_exception();
            }
            throw;
        } catch (...) {
            failure_ = std::current_exception();
            throw;
        }
    }

    // Chunks already appended stay in bytes_ when a read is interrupted.
    task<std::string_view> read_all(operation_options operation) {
        const operation_steps steps(std::move(operation));
        while (auto chunk = co_await read(steps.next("request body read"))) {
            inbound_append(bytes_, reservation_, std::string_view(reinterpret_cast<const char*>(chunk->data()), chunk->size()));
        }
        co_return std::string_view(bytes_);
    }

    task<void> discard(operation_options operation) {
        const operation_steps steps(std::move(operation));
        while (co_await read(steps.next("request body read"))) {
        }
    }

private:
    body_reader* source_;
    std::optional<std::size_t> limit_;
    std::size_t consumed_{0};
    std::string bytes_;
    inbound_buffer_reservation reservation_;
    std::exception_ptr failure_;
};

}  // namespace

// A router's resolution of the request it dispatches. HTTP/2 and HTTP/3 name
// a CONNECT target in :authority and an extended CONNECT protocol in
// :protocol (RFC 8441, RFC 9220); HTTP/1 routes by its request line.
[[nodiscard]] route_resolution resolve_dispatched_request(const route_table& table, const http_request& request) noexcept {
    const auto version = request.protocol_version();
    if (version != http_protocol_version::http2 && version != http_protocol_version::http3) {
        return table.resolve(request);
    }
    const auto protocol = request.protocol();
    return table.resolve(route_request_view{
        .known_method_ = request.known_method(),
        .method_ = request.method(),
        .path_ = request.path(),
        .connect_target_ = request.authority(),
        .extended_protocol_ = http_ascii_equals_ignore_case(protocol, "websocket") ? std::string_view("websocket") : protocol,
    });
}

class router_state final {
public:
    explicit router_state(worker_ref worker) :
        worker_(std::move(worker)) {
        if (!worker_.valid()) {
            throw std::invalid_argument("router requires a valid worker");
        }
    }

    router_state(const router_state&) = delete;
    router_state& operator=(const router_state&) = delete;

    // The last owner releases the state on the router's worker, which is where
    // the lent worker states were adopted.
    ~router_state() {
        if (worker_states_) {
            worker_states_->shutdown();
        }
    }

    void require_current() const {
        if (!worker_.is_current()) {
            throw std::logic_error("router operation must run on its owner worker");
        }
    }

    void require_editable() const {
        require_current();
        if (phase_ != phase::editable) {
            throw std::logic_error("router configuration is sealed or being frozen");
        }
    }

    struct tunnel_upgrade final {
        std::string protocol_;
        http_tunnel_route_config config_;
    };

    struct registration final {
        std::string method_;
        std::string path_;
        std::variant<router_handler, router_stream_handler> handler_;
        // A WebSocket or tunnel route keeps the configuration its endpoint is
        // validated and built from at freeze.
        std::variant<std::monostate, websocket_route_config, tunnel_upgrade> upgrade_;
        std::vector<controller_middleware_descriptor> modifiers_;

        template <typename handler_type, typename upgrade_type = std::monostate>
        registration(std::string_view method, std::string_view path, handler_type handler, std::span<const controller_middleware_descriptor> modifiers, upgrade_type upgrade = {}) :
            method_(method),
            path_(path),
            handler_(std::move(handler)),
            upgrade_(std::move(upgrade)),
            modifiers_(modifiers.begin(), modifiers.end()) {
            if (!is_valid_http_method_token(method)) {
                throw std::invalid_argument("route method must be a valid HTTP token");
            }
            if (!std::visit([](const auto& value) {
                return static_cast<bool>(value);
            }, handler_)) {
                throw std::invalid_argument("route handler must not be empty");
            }
        }
    };

    [[nodiscard]] static route_endpoint make_endpoint(const registration& route, route_stream_handler::invoke_type invoke, request_body_mode body_mode) {
        if (const auto* websocket = std::get_if<websocket_route_config>(&route.upgrade_)) {
            return route_endpoint::websocket(invoke, *websocket);
        }
        if (const auto* tunnel = std::get_if<tunnel_upgrade>(&route.upgrade_)) {
            return route_endpoint::tunnel(invoke, tunnel->protocol_, tunnel->config_);
        }
        return route_endpoint::response_stream(invoke, body_mode);
    }

    void freeze() {
        require_editable();
        phase_ = phase::freezing;
        try {
            detail::router builder;
            std::vector<void*> targets;
            targets.reserve(registrations_.size());
            for (const auto& route : registrations_) {
                const auto method = classify_http_method(route.method_);
                const auto token = method == http_known_method::unknown ? std::string_view(route.method_) : std::string_view{};
                const auto body_mode = std::ranges::any_of(route.modifiers_, [](const auto& value) {
                    return value.stream_request_body();
                })
                    ? request_body_mode::stream
                    : request_body_mode::buffered;
                std::visit([&](const auto& handler) {
                    const auto view = callback_access::ref(handler);
                    auto endpoint = [&] {
                        if constexpr (std::same_as<std::remove_cvref_t<decltype(handler)>, router_handler>) {
                            return route_endpoint::buffered(callback_access::invoke(view), body_mode);
                        } else {
                            return make_endpoint(route, callback_access::invoke(view), body_mode);
                        }
                    }();
                    builder.register_owned_endpoint(targets.size(), method, token, route.path_, std::move(endpoint), {}, route.modifiers_);
                    targets.push_back(callback_access::target(view));
                }, route.handler_);
            }
            configuration_.apply(builder);
            auto plan = builder.compile();
            auto table = std::make_unique<route_table>(*plan, std::span<void* const>(targets));
            const auto route_limits = table->has_route_rate_limit() ? route_rate_limit_presence::present : route_rate_limit_presence::absent;
            auto limiter = rate_limit_ || route_limits == route_rate_limit_presence::present ? std::make_unique<rate_limiter>(rate_limit_ ? std::optional<rate_limit_rule>(rate_limit_->rule_) : std::nullopt, route_limits, rate_limit_ ? rate_limit_->capacity_per_worker_ : default_rate_limit_capacity_per_worker) : std::unique_ptr<rate_limiter>{};
            if (!worker_state_definitions_.empty()) {
                worker_states_.emplace(worker_state_definitions_);
                try {
                    worker_states_->initialize();
                } catch (...) {
                    worker_states_.reset();
                    throw;
                }
            }
            plan_ = std::move(plan);
            table_ = std::move(table);
            limiter_ = std::move(limiter);
            phase_ = phase::frozen;
        } catch (...) {
            phase_ = phase::editable;
            throw;
        }
    }

    static task<void> dispatch(std::shared_ptr<router_state> self, request_stream& stream) {
        self->require_current();
        if (self->phase_ != phase::frozen || stream.worker().id() != self->worker_.id()) {
            throw std::logic_error("dispatch requires a frozen router on the request's worker");
        }
        auto& context_value = stream.context_;
        if (stream.routed_ || stream.response_.committed() || context_access::output_committed(context_value) || context_access::has_response(context_value)) {
            throw std::logic_error("request routing or response selection has already begun");
        }
        stream.routed_ = true;
        const auto& table = *self->table_;
        const auto& request = stream.request();
        const auto resolution = resolve_dispatched_request(table, request);
        const auto* resolved = resolution.resolved();
        context_access::route_binding binding(context_value, resolved ? resolved->route().path() : std::string_view{}, resolved ? resolved->route().param_names() : std::span<const std::string_view>{}, resolved ? resolved->match().values() : std::span<const std::string_view>{}, resolved ? reinterpret_cast<std::uintptr_t>(&resolved->route()) : 0);
        auto& services = context_access::services(context_value);
        services = services.with_request_stream({});
        services.bind_routes(table);
        if (self->limiter_) {
            services.bind_route_rate_limiter(*self->limiter_);
        }
        services.bind_router_capabilities(self->clients_.empty() ? nullptr : &self->clients_, self->worker_states_ ? &*self->worker_states_ : nullptr);
        binding.bind_worker_states();
        // Only this router's own rule: the host's application-wide rule never
        // covers a request stream.
        if (const auto rejection = decide_request_rate_limit(self->limiter_.get(), context_value.conn().client().address(), services.pinned_arrival())) {
            auto response = co_await table.handle_error(request, rate_limit_rejection_error(), services);
            apply_rate_limit_rejection_headers(response, *rejection);
            context_access::set_response(context_value, std::move(response));
            co_return;
        }
        if (resolved == nullptr) {
            auto buffered_services = services;
            buffered_services.response_output() = {};
            http_response response;
            co_await table.dispatch_buffered_response(request, resolution, document_root_binding::none(), buffered_services, static_file_selection_mode::identity_only, response);
            stream.respond(std::move(response));
            co_return;
        }

        const auto& route = resolved->route();
        if (route.endpoint().websocket() != nullptr || route.endpoint().tunnel() != nullptr) {
            co_await dispatch_upgrade(table, stream, *resolved, services);
            co_return;
        }
        services.bind_route_body_limit(route.max_request_body_bytes());
        const auto* buffered = route.endpoint().buffered();
        const auto mode = buffered ? buffered->request_body_mode() : route.endpoint().response_stream()->request_body_mode();
        const auto limit = mode == request_body_mode::buffered
            ? std::optional<std::size_t>(services.max_decoded_request_body_bytes())
            : route.max_request_body_bytes() != 0
            ? std::optional<std::size_t>(route.max_request_body_bytes())
            : std::nullopt;
        const auto request_services = services;
        auto* const body_source = stream.tunnel_requested() ? nullptr : &stream.body();
        std::exception_ptr failure;
        try {
            // The transport already validated and normalized this field. Do not
            // send 100 Continue or run application code for an oversized body.
            if (limit) {
                if (const auto length = request.header("Content-Length")) {
                    std::size_t declared = 0;
                    const auto parsed = std::from_chars(length->data(), length->data() + length->size(), declared);
                    if (parsed.ec == std::errc::result_out_of_range || (parsed.ec == std::errc{} && declared > *limit)) {
                        throw http_request_body_protocol_error(http_request_body_failure::too_large);
                    }
                }
            }
            routed_body body(body_source, limit, services.inbound_buffers());
            auto reader = make_body_reader_facade(body);
            auto loader = make_request_body_loader_facade(body);
            if (mode == request_body_mode::stream) {
                services.bind_streaming_request_body(reader);
            } else {
                services.bind_lazy_request_body(loader);
            }
            if (buffered != nullptr) {
                const auto output = services.response_output();
                services.response_output() = {};
                auto response = co_await table.dispatch_bound_buffered(*resolved, context_value);
                services.response_output() = output;
                context_access::set_response(context_value, std::move(response));
            } else {
                auto response = co_await table.dispatch_bound_stream(*resolved, context_value, resolved->stream_handler());
                if (response) {
                    context_access::set_response(context_value, std::move(*response));
                }
            }
            co_return;
        } catch (...) {
            failure = std::current_exception();
        }
        if (stream.response_.committed() || context_access::output_committed(context_value)) {
            std::rethrow_exception(failure);
        }
        // Body adapters above have retired. The request-only error path must
        // not retain their borrowed views while constructing its fallback.
        services = request_services;
        auto response = co_await table.handle_exception(request, failure, services);
        context_access::set_response(context_value, std::move(response));
    }

    worker_ref worker_;
    app_configuration configuration_;
    std::vector<registration> registrations_{};
    std::optional<rate_limit_config> rate_limit_;
    borrowed_clients clients_;
    std::vector<worker_state_definition> worker_state_definitions_{};

private:
    enum class phase { editable,
        freezing,
        frozen };

    // The route's middleware runs before its terminal accepts the upgrade
    // through the request stream, binds the session to the context and runs
    // the handler on it; the transport finishes the session afterwards.
    static task<void> run_upgrade_terminal(request_stream& stream, const resolved_route& resolved, context& context_value) {
        const auto& endpoint = resolved.route().endpoint();
        if (const auto* websocket_endpoint = endpoint.websocket()) {
            const auto accepted = co_await stream.accept_websocket_endpoint(*websocket_endpoint);
            auto* session = accepted.session();
            // A rejected handshake already selected its error response, whose
            // status the access log records when the server sends it.
            if (session == nullptr) {
                co_return;
            }
            context_websocket_binding session_binding(context_value, *session);
            co_await resolved.stream_handler()(context_value);
            co_return;
        }
        const auto status = stream.request().known_method() == http_known_method::connect ? http_status::ok : http_status::switching_protocols;
        auto* tunnel = co_await stream.accept_tunnel_impl(status, endpoint.tunnel()->config());
        context_tunnel_binding tunnel_binding(context_value, *tunnel);
        co_await resolved.stream_handler()(context_value);
    }

    static task<void> dispatch_upgrade(const route_table& table, request_stream& stream, const resolved_route& resolved, const context_services& services) {
        auto& context_value = stream.context_;
        const auto& request = stream.request();
        const bool websocket_route = resolved.route().endpoint().websocket() != nullptr;
        // The route exists only for the upgrade its endpoint names: a plain
        // GET, or an upgrade request carrying content, cannot reach it.
        if (websocket_route ? stream.accept_websocket_ == nullptr : !stream.tunnel_requested()) {
            auto response = co_await table.handle_error(request, websocket_route ? http_error_info({.status_ = http_status::bad_request, .code_ = "invalid_websocket_handshake", .message_ = "invalid WebSocket handshake"}) : http_error_info({.status_ = http_status::bad_request, .message_ = "tunnel route requires a CONNECT or upgrade request without content"}), services);
            context_access::set_response(context_value, std::move(response));
            co_return;
        }
        auto terminal = [&stream, &resolved](context& terminal_context) {
            return run_upgrade_terminal(stream, resolved, terminal_context);
        };
        std::exception_ptr failure;
        try {
            auto response = co_await table.dispatch_bound_stream(resolved, context_value, callback_access::bind<task<void>(context&)>(terminal));
            if (response) {
                context_access::set_response(context_value, std::move(*response));
            }
            co_return;
        } catch (...) {
            failure = std::current_exception();
        }
        // A failure after the upgrade head ends the established session; the
        // transport reports it.
        if (stream.response_.committed() || context_access::output_committed(context_value)) {
            std::rethrow_exception(failure);
        }
        auto response = co_await table.handle_exception(request, failure, services);
        context_access::set_response(context_value, std::move(response));
    }

    phase phase_{phase::editable};
    compiled_route_plan_ptr_type plan_;
    std::unique_ptr<route_table> table_;
    std::unique_ptr<rate_limiter> limiter_;
    std::optional<worker_state_registry> worker_states_;
};

}  // namespace ruvia::detail

namespace ruvia {

::ruvia::router::router(worker_ref worker) :
    state_(std::make_shared<detail::router_state>(std::move(worker))) {}
::ruvia::router::~router() = default;

::ruvia::router& ::ruvia::router::add_buffered(std::string_view method, std::string_view path, router_handler handler, std::span<const detail::controller_middleware_descriptor> modifiers) {
    state_->require_editable();
    state_->registrations_.emplace_back(method, path, std::move(handler), modifiers);
    return *this;
}

::ruvia::router& ::ruvia::router::add_stream(std::string_view method, std::string_view path, router_stream_handler handler, std::span<const detail::controller_middleware_descriptor> modifiers) {
    state_->require_editable();
    state_->registrations_.emplace_back(method, path, std::move(handler), modifiers);
    return *this;
}

::ruvia::router& ::ruvia::router::add_websocket(std::string_view path, router_stream_handler handler, websocket_route_config config, std::span<const detail::controller_middleware_descriptor> modifiers) {
    state_->require_editable();
    state_->registrations_.emplace_back("GET", path, std::move(handler), modifiers, std::move(config));
    return *this;
}

::ruvia::router& ::ruvia::router::add_tunnel(std::string_view protocol, std::string_view target, router_stream_handler handler, http_tunnel_route_config config, std::span<const detail::controller_middleware_descriptor> modifiers) {
    state_->require_editable();
    state_->registrations_.emplace_back("CONNECT", target, std::move(handler), modifiers, detail::router_state::tunnel_upgrade{std::string(protocol), config});
    return *this;
}

::ruvia::router& ::ruvia::router::add_worker_state(detail::worker_state_definition definition) {
    state_->require_editable();
    detail::append_worker_state_definition(state_->worker_state_definitions_, std::move(definition));
    return *this;
}

::ruvia::router& ::ruvia::router::rate_limit(rate_limit_config config) & {
    state_->require_editable();
    detail::validate_rate_limit_config(config);
    state_->rate_limit_ = config;
    return *this;
}

#ifdef RUVIA_ENABLE_REDIS
::ruvia::router& ::ruvia::router::bind_redis(std::string_view alias, const redis_client& client) & {
    state_->require_editable();
    state_->clients_.bind_redis(state_->worker_, alias, client);
    return *this;
}
#endif

#ifdef RUVIA_ENABLE_DATABASE
::ruvia::router& ::ruvia::router::bind_database(std::string_view alias, const db_client& client) & {
    state_->require_editable();
    state_->clients_.bind_database(state_->worker_, alias, client);
    return *this;
}
#endif

::ruvia::router& ::ruvia::router::bind_http_client(std::string_view alias, const http_client& clients) & {
    state_->require_editable();
    state_->clients_.bind_http_client(state_->worker_, alias, clients);
    return *this;
}

::ruvia::router& ::ruvia::router::add_middleware(detail::owned_middleware_descriptor descriptor) {
    state_->require_editable();
    state_->configuration_.add_middleware(std::move(descriptor));
    return *this;
}

::ruvia::router& ::ruvia::router::add_scoped_middleware(std::string_view prefix, detail::owned_middleware_descriptor descriptor) {
    return add_middleware(std::move(descriptor).scoped_to(detail::normalize_fallback_prefix(prefix)));
}

::ruvia::router& ::ruvia::router::on_error(http_error_handler handler) & {
    state_->require_editable();
    state_->configuration_.on_error(detail::own_callback(std::move(handler)));
    return *this;
}

::ruvia::router& ::ruvia::router::on_not_found(http_not_found_handler handler) & {
    state_->require_editable();
    state_->configuration_.on_not_found(detail::own_callback(std::move(handler)));
    return *this;
}

::ruvia::router& ::ruvia::router::on_error(scoped_error_handler_options options) & {
    state_->require_editable();
    state_->configuration_.on_error(options.prefix_, detail::own_callback(std::move(options.handler_)));
    return *this;
}

::ruvia::router& ::ruvia::router::on_not_found(scoped_not_found_handler_options options) & {
    state_->require_editable();
    state_->configuration_.on_not_found(options.prefix_, detail::own_callback(std::move(options.handler_)));
    return *this;
}

void ::ruvia::router::freeze() & {
    state_->freeze();
}

task<void> ruvia::router::dispatch(request_stream& stream) & {
    return make_scoped_operation(stream.operation_scope_, detail::router_state::dispatch(state_, stream));
}

// The route-registration surface a controller sees. Each call builds the
// endpoint its route kind runs and hands it, with the scope's prefix and
// middlewares, to the router's single registration path.

using namespace detail;

namespace {

[[nodiscard]] std::size_t write_joined_controller_path(char* destination, std::string_view prefix, std::string_view path) noexcept {
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
    std::string heap_{};
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

[[nodiscard]] std::string join_controller_paths(std::string_view prefix, std::string_view path) {
    scoped_controller_path joined(prefix, path);
    if (!joined.heap_.empty()) {
        return std::move(joined.heap_);
    }
    return std::string(joined.view_);
}
[[nodiscard]] std::vector<controller_middleware_descriptor> merge_middleware_descriptors(std::span<const controller_middleware_descriptor> base, std::span<const controller_middleware_descriptor> extra) {
    std::vector<controller_middleware_descriptor> middlewares;
    middlewares.reserve(base.size() + extra.size());
    middlewares.insert(middlewares.end(), base.begin(), base.end());
    middlewares.insert(middlewares.end(), extra.begin(), extra.end());
    return middlewares;
}

[[nodiscard]] request_body_mode declared_request_body_mode(std::span<const controller_middleware_descriptor> controller_middlewares, std::span<const controller_middleware_descriptor> route_middlewares) noexcept {
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

detail::controller_route_builder::controller_route_builder(router& router_value, std::string_view prefix, std::vector<controller_middleware_descriptor> middlewares) :
    controller_route_builder(router_value, join_controller_paths({}, prefix), std::move(middlewares), owned_prefix_tag_type{}) {}

detail::controller_route_builder::controller_route_builder(router& router_value, std::string prefix, std::vector<controller_middleware_descriptor> middlewares, owned_prefix_tag_type) :
    router_(router_value),
    prefix_(std::move(prefix)),
    middlewares_(std::move(middlewares)) {}

detail::controller_route_builder::~controller_route_builder() = default;

void detail::controller_route_builder::register_route(http_known_method method, std::string_view method_token, std::string_view path, route_handler handler, std::span<const controller_middleware_descriptor> middlewares) const {
    const auto body_mode = declared_request_body_mode(middlewares_, middlewares);
    const scoped_controller_path joined(prefix_, path);
    router_.register_endpoint(method, method_token, joined.view_, route_endpoint::buffered(callback_access::invoke(handler), body_mode), callback_access::target(handler), middlewares_, middlewares);
}

void detail::controller_route_builder::register_route(http_known_method method, std::string_view method_token, std::string_view path, route_stream_handler handler, std::span<const controller_middleware_descriptor> middlewares) const {
    const auto body_mode = declared_request_body_mode(middlewares_, middlewares);
    const scoped_controller_path joined(prefix_, path);
    router_.register_endpoint(method, method_token, joined.view_, route_endpoint::response_stream(callback_access::invoke(handler), body_mode), callback_access::target(handler), middlewares_, middlewares);
}

void detail::controller_route_builder::register_tunnel_route(std::string_view protocol, std::string_view target, route_stream_handler handler, std::span<const controller_middleware_descriptor> middlewares, http_tunnel_route_config config) const {
    std::optional<scoped_controller_path> joined;
    if (!protocol.empty()) {
        joined.emplace(prefix_, target);
    }
    const auto path = protocol.empty() ? target : joined->view_;
    router_.register_endpoint(http_known_method::connect, {}, path, route_endpoint::tunnel(callback_access::invoke(handler), protocol, config), callback_access::target(handler), middlewares_, middlewares);
}

void detail::controller_route_builder::register_websocket_route(http_known_method method, std::string_view path, route_stream_handler handler, std::span<const controller_middleware_descriptor> middlewares, websocket_route_config websocket_config) const {
    const scoped_controller_path joined(prefix_, path);
    router_.register_endpoint(method, {}, joined.view_, route_endpoint::websocket(callback_access::invoke(handler), std::move(websocket_config)), callback_access::target(handler), middlewares_, middlewares);
}
detail::controller_route_builder detail::controller_route_builder::create_scope(std::string_view prefix, std::span<const controller_middleware_descriptor> middlewares) const {
    auto merged = merge_middleware_descriptors(middlewares_, middlewares);
    return controller_route_builder(router_, join_controller_paths(prefix_, prefix), std::move(merged), owned_prefix_tag_type{});
}

namespace detail {
namespace {

struct controller_registry_state final {
    std::mutex mutex_;
    std::vector<controller_registrar> registrars_{};
    bool sealed_{false};
};

controller_registry_state& controller_registry() {
    static controller_registry_state state;
    return state;
}

}  // namespace

controller_store::controller_store() = default;

controller_store::controller_store(std::span<const controller_registrar> registrars) {
    lifetimes_.reserve(registrars.size());
    for (const auto& registrar : registrars) {
        registrar.create_(*this);
    }
}

controller_store::~controller_store() = default;

controller_store::controller_store(controller_store&&) noexcept = default;

controller_store& controller_store::operator=(controller_store&&) noexcept = default;

std::size_t controller_store::size() const noexcept {
    return lifetimes_.size();
}

void* controller_store::instance(std::size_t slot) const noexcept {
    return lifetimes_[slot].target_;
}

void controller_store::add_lifetime(void* target, destroy_type destroy) {
    lifetimes_.emplace_back(target, destroy);
}

bool add_controller_registrar(controller_registrar registrar_value) {
    auto& state_value = controller_registry();
    std::lock_guard lock(state_value.mutex_);
    if (state_value.sealed_) {
        throw std::logic_error(
            "controller registration is sealed; load every controller module before application::run() or "
            "test_app::request()");
    }
    for (const auto& existing : state_value.registrars_) {
        if (existing == registrar_value) {
            return true;
        }
    }
    state_value.registrars_.push_back(registrar_value);
    return true;
}

std::vector<controller_registrar> seal_controller_registrars() {
    std::vector<controller_registrar> registrars{};
    auto& state_value = controller_registry();
    std::lock_guard lock(state_value.mutex_);
    state_value.sealed_ = true;
    registrars = state_value.registrars_;
    return registrars;
}

}  // namespace detail

}  // namespace ruvia
