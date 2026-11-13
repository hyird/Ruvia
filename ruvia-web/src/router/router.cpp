#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/core/fnv1a.h"
#include "ruvia/http/http_connect_udp.h"
#include "ruvia/http/http_known_method.h"
#include "ruvia/http/http_request_target.h"
#include "ruvia/http/url_encoding.h"
#include "ruvia/web/detail/app_configuration.h"

#include "router/route_table.h"
#include "router/router_impl.h"

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
    return !segment.empty() && segment.size() <= 6 &&
           (uri_paths_equivalent(segment, ".") || uri_paths_equivalent(segment, ".."));
}

// Build, reverse routing and request matching share the same segments.
// Each remainder retains its leading slash, preserving empty segments without
// allowing them to match a parameter. A final slash yields a trailing empty
// segment, so "/a/" and "/a" are distinct paths (RFC 3986) for dynamic routes
// exactly as they are for byte-exact static routes.
[[nodiscard]] inline bool split_path_segment(
    std::string_view path, std::string_view& segment, std::string_view& rest) noexcept {
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
[[nodiscard]] inline bool route_patterns_equivalent(
    std::string_view left, std::string_view right, bool compare_param_names) noexcept {
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
        if (kind != classify_route_segment(right_segment) ||
            ((kind == route_segment_kind::literal || compare_param_names) &&
                !uri_paths_equivalent(left_segment, right_segment))) {
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

detail::router::router()
    : plan_(std::make_unique<compiled_route_plan>()),
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

void detail::router::set_prefix_error_handlers(
    std::span<const std::pair<std::string, callback_registration<http_error_handler>>> handlers) {
    replace_prefix_handlers(plan_->prefix_error_handlers_, handlers);
}

void detail::router::set_prefix_not_found_handlers(
    std::span<const std::pair<std::string, callback_registration<http_not_found_handler>>> handlers) {
    replace_prefix_handlers(plan_->prefix_not_found_handlers_, handlers);
}

void detail::router::set_global_middlewares(std::span<const controller_middleware_descriptor> descriptors) {
    for (const auto& descriptor : descriptors) {
        plan_->has_route_rate_limit_ = plan_->has_route_rate_limit_ || descriptor.uses_route_rate_limit();
    }
    global_middleware_descriptors_.assign(descriptors.begin(), descriptors.end());
}

std::size_t detail::router::append_route_middlewares(
    std::span<const controller_middleware_descriptor> controller_middlewares,
    std::span<const controller_middleware_descriptor> route_middlewares) {
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
            if (key != nullptr &&
                std::ranges::any_of(std::span(route_middleware_descriptors_).subspan(offset),
                    [key](const auto& registered) noexcept { return registered.validated_model_type_key() == key; })) {
                throw std::invalid_argument("duplicate validated model type on route");
            }
            plan_->has_route_rate_limit_ = plan_->has_route_rate_limit_ || descriptor.uses_route_rate_limit();
            route_middleware_descriptors_.push_back(descriptor);
        }
    }
    route_middleware_ranges_.push_back({offset, route_middleware_descriptors_.size() - offset});
    return body_limit;
}

void detail::router::declare_controllers(
    std::span<const controller_registrar> registrars, const controller_store& controllers) {
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

void detail::router::validate_route_target(
    http_known_method method, std::string_view method_token, std::string_view path, const route_endpoint& endpoint) const {
    if (const auto* tunnel = endpoint.tunnel()) {
        if (method != http_known_method::connect || !method_token.empty()) {
            throw std::invalid_argument("tunnel route requires CONNECT");
        }
        if (tunnel->protocol().empty()) {
            if (path != "*" && !is_valid_http_connect_authority(path)) {
                throw std::invalid_argument("CONNECT route requires host:port authority or *");
            }
        } else if ((path.find('?') != std::string_view::npos) || !is_valid_http_origin_form_target(path) ||
                   has_dot_segment(path)) {
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

detail::compiled_route_plan_ptr_type detail::compile_route_plan(
    std::span<const controller_registrar> registrars, const controller_store& controllers,
    const app_configuration& configuration) {
    router routes_value;
    routes_value.declare_controllers(registrars, controllers);
    configuration.apply(routes_value);
    return routes_value.compile();
}

void detail::router::register_endpoint(http_known_method method, std::string_view method_token,
    std::string_view path, route_endpoint endpoint, void* handler_target,
    std::span<const controller_middleware_descriptor> controller_middlewares,
    std::span<const controller_middleware_descriptor> route_middlewares) {
    if (controller_target_ == nullptr) {
        throw std::logic_error("routes are declared only while compiling the route plan");
    }
    if (handler_target != controller_target_) {
        throw std::logic_error("route handler must be bound to the controller declaring it");
    }
    register_owned_endpoint(controller_slot_, method, method_token, path, std::move(endpoint),
        controller_middlewares, route_middlewares);
}

void detail::router::register_owned_endpoint(std::size_t slot, http_known_method method, std::string_view method_token,
    std::string_view path, route_endpoint endpoint,
    std::span<const controller_middleware_descriptor> controller_middlewares,
    std::span<const controller_middleware_descriptor> route_middlewares) {
    if (!method_token.empty()) {
        if (!is_valid_http_method_token(method_token)) {
            throw std::invalid_argument("extension route method must be a valid HTTP method token");
        }
        if (classify_http_method(method_token) != http_known_method::unknown) {
            throw std::invalid_argument(
                "extension route method is a known method; register it with its typed route macro");
        }
    }
    validate_route_target(method, method_token, path, endpoint);

    auto& routes = plan_->routes_;
    const auto* tunnel = endpoint.tunnel();
    auto hash = route_table::route_hash(
        method, route_table::path_hash(tunnel == nullptr ? std::string_view(path) : tunnel->protocol()));
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
            if (registered_tunnel != nullptr && registered_tunnel->protocol() == tunnel->protocol() &&
                (tunnel->protocol().empty()
                        ? registered.path() == path || http_authorities_equal(path, registered.path(), 0)
                        : route_patterns_equivalent(registered.path(), path, /*compare_param_names=*/true))) {
                throw std::invalid_argument("duplicate CONNECT route registration");
            }
        } else if (route_patterns_equivalent(registered.path(), path, /*compare_param_names=*/true)) {
            throw std::invalid_argument("duplicate route registration");
        }
    }

    const bool dynamic = (tunnel == nullptr || !tunnel->protocol().empty()) && is_dynamic_path(path);
    const auto index = routes.size();
    const auto max_request_body_bytes =
        append_route_middlewares(controller_middlewares, route_middlewares);
    routes.emplace_back(route_entry::init_type{.index_ = index,
        .controller_ = slot,
        .method_ = method,
        .method_token_ = method_token,
        .path_ = path,
        .endpoint_ = std::move(endpoint),
        .dynamic_ = dynamic,
        .max_request_body_bytes_ = max_request_body_bytes});
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
        plan.extension_paths_.push_back({.path_ = path,
            .methods_ = std::span<const std::string_view>(plan.extension_methods_).subspan(offset)});
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

}  // namespace ruvia

namespace ruvia {
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
[[nodiscard]] middleware_scope_coverage route_scope_coverage(
    std::string_view pattern, bool dynamic, std::string_view prefix) noexcept {
    if (!dynamic) {
        // A static route serves exactly its own path.
        return detail::path_is_under_prefix(pattern, prefix) ? middleware_scope_coverage::full
                                                             : middleware_scope_coverage::none;
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

detail::route_table::route_table(
    const compiled_route_plan& plan, const controller_store& controllers)
    : route_table(plan) {
    handler_targets_.reserve(plan.routes_.size());
    for (const auto& route : plan.routes_) {
        handler_targets_.push_back(controllers.instance(route.controller()));
    }
    build_middleware_instances();
}

detail::route_table::route_table(const compiled_route_plan& plan, std::span<void* const> targets)
    : route_table(plan) {
    handler_targets_.reserve(plan.routes_.size());
    for (const auto& route : plan.routes_) {
        if (route.controller() >= targets.size() || targets[route.controller()] == nullptr) {
            throw std::invalid_argument("route handler slot is not bound");
        }
        handler_targets_.push_back(targets[route.controller()]);
    }
    build_middleware_instances();
}

detail::route_table::route_table(const compiled_route_plan& plan)
    : plan_(&plan),
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
        middleware_frames_.push_back(callback_access::make<task<void>(context&, next&)>(
            middleware_instances_[instance], plan.middleware_instances_[instance].invoke()));
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
            if ((left_tunnel == nullptr) != (right_tunnel == nullptr) ||
                (left_tunnel != nullptr && left_tunnel->protocol() != right_tunnel->protocol())) {
                continue;
            }
            if (route_patterns_equivalent(left.path(), right.path(), /*compare_param_names=*/false)) {
                throw std::invalid_argument(("conflicting dynamic routes match the same requests with equal precedence: " +
                                             describe_route(left.method(), left.path()) + " and " +
                                             describe_route(right.method(), right.path()))
                        .c_str());
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
    plan.middleware_instances_.insert(plan.middleware_instances_.end(), route_middleware_descriptors_.begin(),
        route_middleware_descriptors_.end());

    const auto frame_capacity =
        plan.routes_.size() * global_count + route_middleware_descriptors_.size() + global_count;
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

}  // namespace ruvia

namespace ruvia {
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
            auto found = std::ranges::find(
                plan.connect_protocols_, protocol, &compiled_route_plan::connect_protocol_index_type::protocol_);
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

    const auto offset =
        names.empty() ? names_storage.size() : static_cast<std::size_t>(names.data() - names_storage.data());
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
                node_value->static_children_.push_back(
                    compiled_route_plan::dynamic_static_child_type{segment, child_node});
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

}  // namespace ruvia

// Substitute parameters and percent-encode path values while preserving the
// same segments used to build and resolve the registered route.

namespace ruvia {

namespace {

// RFC 3986 pchar minus pct-encoded: bytes a path segment may carry verbatim.
[[nodiscard]] constexpr bool is_url_for_segment_byte(char value) noexcept {
    return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') ||
           (value >= '0' && value <= '9') || value == '-' || value == '.' || value == '_' ||
           value == '~' || value == '!' || value == '$' || value == '&' || value == '\'' ||
           value == '(' || value == ')' || value == '*' || value == '+' || value == ',' ||
           value == ';' || value == '=' || value == ':' || value == '@';
}

[[nodiscard]] constexpr std::size_t url_for_encoded_size(
    std::string_view value, bool keep_slashes) noexcept {
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

std::string detail::route_table::url_for(std::string_view pattern,
    std::span<const std::string_view> values) const {
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

}  // namespace ruvia

namespace ruvia {

detail::route_handler detail::resolved_route::handler() const noexcept {
    return callback_access::make<task<http_response>(context&)>(
        handler_target_, route_->endpoint().buffered()->invoke());
}

detail::route_stream_handler detail::resolved_route::stream_handler() const noexcept {
    return callback_access::make<task<void>(context&)>(handler_target_, route_->endpoint().stream_invoke());
}

detail::route_resolution detail::route_table::resolved(
    const route_entry& route) const noexcept {
    return route_resolution::resolved(route, handler_targets_[route.index()]);
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
    const auto& indices = plan_->extension_route_indices_;
    const auto found = std::lower_bound(indices.begin(), indices.end(), std::pair{method_token, path},
        [this](std::size_t index, const auto& key) noexcept {
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
    const auto found = std::ranges::lower_bound(indices, method_token, {},
        [this](std::size_t index) noexcept { return plan_->routes_[index].method_token(); });
    return found != indices.end() && plan_->routes_[*found].method_token() == method_token;
}

std::span<const std::string_view> detail::route_table::extension_methods_for(std::string_view path) const noexcept {
    const auto& paths = plan_->extension_paths_;
    const auto found = std::ranges::lower_bound(paths, path, [](std::string_view left, std::string_view right) noexcept { return compare_uri_paths(left, right) < 0; }, &compiled_route_plan::extension_path_type::path_);
    return found != paths.end() && uri_paths_equivalent(found->path_, path)
               ? found->methods_
               : std::span<const std::string_view>{};
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
    std::uint32_t excluded_mask = is_routable_method(method) ? (1U << method_index(method)) : 0U;
    std::uint32_t initial_mask = 0;
    if (method == http_known_method::head) {
        const auto get_bit = 1U << method_index(http_known_method::get);
        const auto* fallback = (plan_->static_method_mask_ & get_bit) != 0
                                   ? find_static_route(http_known_method::get, path, hash)
                                   : nullptr;
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
    return route.method() == http_known_method::get &&
           (endpoint.buffered() != nullptr || endpoint.response_stream() != nullptr);
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

std::uint64_t detail::route_table::route_hash(
    http_known_method method, std::uint64_t hash) noexcept {
    return hash ^ (static_cast<std::uint64_t>(method) * fnv1a_prime);
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

std::uint32_t detail::route_table::allowed_methods(std::string_view path, std::uint64_t hash,
    std::uint32_t excluded_mask, std::uint32_t initial_mask) const noexcept {
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

std::size_t detail::route_table::find_dynamic_node(
    const dynamic_node_type& node_value, std::string_view path, route_match& match) noexcept {
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
        if (const auto route_index = find_dynamic_node(*static_child->node_, rest, match);
            route_index != no_route_index) {
            return route_index;
        }
        match.truncate(original_param_count);
    }

    if (node_value.param_child_ && !segment.empty() && match.add(segment)) {
        if (const auto route_index = find_dynamic_node(*node_value.param_child_, rest, match);
            route_index != no_route_index) {
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

std::size_t detail::route_table::find_dynamic_node_no_params(
    const dynamic_node_type& node_value, std::string_view path) noexcept {
    std::string_view segment;
    std::string_view rest;
    if (!split_path_segment(path, segment, rest)) {
        return node_value.route_index_ != no_route_index ? node_value.route_index_ : node_value.wildcard_route_index_;
    }

    if (const auto* static_child = find_dynamic_static_child(node_value, segment); static_child != nullptr) {
        if (const auto route_index = find_dynamic_node_no_params(*static_child->node_, rest);
            route_index != no_route_index) {
            return route_index;
        }
    }

    if (node_value.param_child_ != nullptr && !segment.empty()) {
        if (const auto route_index = find_dynamic_node_no_params(*node_value.param_child_, rest);
            route_index != no_route_index) {
            return route_index;
        }
    }

    return node_value.wildcard_route_index_;
}

const detail::route_table::dynamic_static_child_type* detail::route_table::find_dynamic_static_child(
    const dynamic_node_type& node_value, std::string_view segment) noexcept {
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

    const auto iter = std::ranges::partition_point(node_value.static_children_,
        [segment](const dynamic_static_child_type& child_value) noexcept {
            return compare_uri_paths(child_value.segment_, segment) < 0;
        });
    if (iter != node_value.static_children_.end() && uri_paths_equivalent(iter->segment_, segment)) {
        return &*iter;
    }
    return nullptr;
}

const detail::route_entry* detail::route_table::find_dynamic_route(
    http_known_method method, std::string_view path, route_match& match) const noexcept {
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

}  // namespace ruvia
