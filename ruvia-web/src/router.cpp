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

#include "ruvia/core/event_loop.h"
#include "ruvia/core/worker_ref.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_response.h"
#include "ruvia/web/app.h"
#include "ruvia/web/context.h"
#include "ruvia/web/http_client.h"
#include "ruvia/web/middleware.h"

#include "route_table.h"

#ifdef RUVIA_ENABLE_DATABASE
#include "ruvia/web/db/db.h"
#endif
#ifdef RUVIA_ENABLE_REDIS
#include "ruvia/web/redis/redis.h"
#endif

#include <chrono>
#include <compare>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <limits>
#include <random>
#include <type_traits>

#include "ruvia/core/task.h"
#include "ruvia/core/worker_ref.h"
#include "ruvia/http/http_client.h"
#include "ruvia/web/model.h"

#ifdef RUVIA_ENABLE_REDIS
#include "ruvia/http/http_header.h"
#include "ruvia/web/middleware.h"
#include "ruvia/web/redis/redis.h"

#include "route_table.h"
#endif

#include <atomic>
#include <ctime>

#include <asio.hpp>
#include <asio/error.hpp>
#include <asio/ssl/error.hpp>
#include <asio/system_error.hpp>
#include <openssl/bio.h>
#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/params.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include "ruvia/web/app.h"
#include "ruvia/web/db/db.h"
#include "ruvia/web/redis/redis.h"

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

// Choosing what answers a request: the matched route, a 405 with Allow, the
// document-root fallback, or nothing -- and running a buffered handler once one
// is chosen.

namespace ruvia {

namespace {

http_response make_allow_no_content_response(std::uint32_t method_mask,
    std::span<const std::string_view> extension_methods) {
    http_response response;
    response.status(ruvia::http_status::no_content);
    response.allow_methods(method_mask, extension_methods);
    return response;
}

// Methods a document-root file supports: the fallback serves GET and HEAD and
// answers OPTIONS; every other known method is 405 with this Allow set.
constexpr std::uint32_t document_root_method_mask =
    (1U << static_cast<unsigned>(http_known_method::get)) | (1U << static_cast<unsigned>(http_known_method::head)) |
    (1U << static_cast<unsigned>(http_known_method::options));

// Outcome of probing the document root for an unrouted request: nothing there,
// a complete GET/HEAD response, or an existing resource asked with another
// method, which the dispatcher answers with 405 or an OPTIONS Allow set.
struct document_root_probe final {
    std::optional<detail::static_root_entry_view> entry_;
    std::string_view relative_{};
};

[[nodiscard]] document_root_probe probe_document_root_resource(
    const detail::document_root_binding& document_root, const http_request& request,
    std::optional<std::string>& decoded,
    std::optional<std::string>& storage) {
    const auto* const root = document_root.root();
    // Only an origin-form path names a document-root resource: a CONNECT
    // authority or "OPTIONS *" reaches here unrouted as well and must not be
    // probed as a file name. resolve() has already rejected dot segments.
    // static_file takes a decoded file path: decode it here, once, so a name
    // holding an encoded octet ("%20", UTF-8) resolves (RFC 3986 2.1, 6.2.2.2).
    if (root == nullptr || !request.path().starts_with('/') ||
        request.path().find("//") != std::string_view::npos) {
        return {};
    }
    const auto path = detail::decode_request_path(request.path(), decoded.emplace());
    if (!path) {
        return {};
    }
    auto relative = path->substr(1);
    auto entry_value = detail::static_root_access::try_resolve(
        *root, relative, storage.emplace());
    if (!entry_value.has_value()) {
        return {};
    }
    return {.entry_ = entry_value, .relative_ = relative};
}

}  // namespace

task<std::optional<http_response>> detail::route_table::dispatch_response_stream(
    const http_request& request, const resolved_route& resolved,
    response_stream_writer& response_stream, const context_services& services) const {
    if (resolved.route().endpoint().response_stream() == nullptr) {
        throw std::logic_error("route is not a response stream route");
    }
    return dispatch_stream_route(
        request, resolved, resolved.stream_handler(), services, &response_stream);
}

task<std::optional<http_response>> detail::route_table::dispatch_upgrade(const http_request& request,
    const resolved_route& resolved, const route_stream_handler& handler,
    const context_services& services) const {
    const auto& endpoint = resolved.route().endpoint();
    if (endpoint.tunnel() == nullptr && endpoint.websocket() == nullptr) {
        throw std::logic_error("route is not an upgrade route");
    }
    return dispatch_stream_route(request, resolved, handler, services);
}

task<void> detail::route_table::dispatch_buffered_response(const http_request& request,
    const route_resolution& resolution, document_root_binding document_root,
    const context_services& services, static_file_selection_mode static_file_mode,
    http_response& response) const {
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
            const auto fallback_probe =
                resolution.rejected() != nullptr || document_root.root() == nullptr
                    ? document_root_probe{}
                    : probe_document_root_resource(
                          document_root, request,
                          decoded_fallback_path, fallback_path_storage);
            const auto method = request.known_method();
            if (fallback_probe.entry_.has_value() &&
                (method == http_known_method::get || method == http_known_method::head)) {
                auto context_value = detail::context_access::make(request, services);
                detail::context_access::services(context_value).bind_routes(*this);
                auto fallback_response = co_await detail::context_access::static_file_resolved(
                    context_value, *document_root.root(), fallback_probe.relative_, {},
                    *fallback_probe.entry_, static_file_mode);
                auto terminal = [&fallback_response](context&) -> task<http_response> {
                    co_return std::move(fallback_response);
                };
                response = co_await run_unmatched_chain(
                    context_value, callback_access::bind<task<http_response>(context&)>(terminal));
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
            if (request.known_method() == http_known_method::unknown &&
                !recognizes_method_token(request.method())) {
                // RFC 9110 15.5.6/15.6.2: 405 says the method is known here but
                // unsupported by this resource; a token no route registered is
                // not known here at all, so it stays 501 whatever the path holds.
                error = http_error_info({.status_ = ruvia::http_status::not_implemented,
                    .message_ = "method not implemented"});
            } else if (request.known_method() == http_known_method::options &&
                       request.path() == "*") {
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
                    error = http_error_info({.status_ = ruvia::http_status::method_not_allowed,
                        .message_ = "method not allowed"});
                }
            }

            if (unmatched_response) {
                auto context_value = detail::context_access::make(request, services);
                detail::context_access::services(context_value).bind_routes(*this);
                auto terminal = [&unmatched_response](context&) -> task<http_response> {
                    co_return std::move(*unmatched_response);
                };
                response = co_await run_unmatched_chain(
                    context_value, callback_access::bind<task<http_response>(context&)>(terminal));
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

task<bool> detail::route_table::dispatch_resolved_route(const http_request& request,
    const resolved_route& resolved, const context_services& services,
    const std::optional<http_request_body_failure>* body_failure, http_response& response) const {
    const auto body_failed = [body_failure] { return body_failure != nullptr && body_failure->has_value(); };
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

task<http_response> detail::route_table::dispatch_bound_buffered(const resolved_route& resolved,
    context& context_value, const std::optional<http_request_body_failure>* body_failure) const {
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

}  // namespace ruvia

// Turning a failed request into a response: the error a thrown exception really
// carries, the metadata that survives onto the response, and the scoped error /
// not-found handlers that get the last word.

namespace ruvia {

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

[[nodiscard]] classified_error classify_exception(
    const std::exception_ptr& exception) {
    try {
        if (exception != nullptr) {
            std::rethrow_exception(exception);
        }
        return {.error_ = http_error({.status_ = ruvia::http_status::internal_server_error,
                    .message_ = "unhandled exception"})};
    } catch (const detail::inbound_transport_error&) {
        throw;
    } catch (const detail::unsupported_request_content_coding& error) {
        return {.error_ = http_error({.status_ = http_unsupported_content_coding::status(),
                    .code_ = "unsupported_content_coding",
                    .message_ = error.what()}),
            .accepted_codings_ = error.accepted_codings()};
    } catch (const detail::rate_limit_rejected& rejected) {
        const auto error = detail::rate_limit_rejection_error();
        return {.error_ = http_error(
                    {.status_ = error.status(), .code_ = error.code(), .message_ = error.message()}),
            .rate_limit_rejection_ = rejected.rejection()};
    } catch (const http_error& error) {
        return {.error_ = http_error(error)};
    } catch (const http_protocol_error& error) {
        return {.error_ = http_error(
                    {.status_ = error.status(), .code_ = error.code(), .message_ = error.what()})};
    } catch (const detail::inbound_buffer_limit_error&) {
        return {.error_ = http_error({.status_ = ruvia::http_status::service_unavailable,
                    .code_ = "inbound_buffer_limit",
                    .message_ = "inbound buffer capacity exhausted"})};
    } catch (const blocking_operation_rejected& error) {
        // The blocking pool refused the work or the worker is going away. That
        // is capacity, not a bug in the request: answer it like any other
        // overload instead of a 500. The message is the framework's own and
        // names no application internals.
        return {.error_ = http_error({.status_ = ruvia::http_status::service_unavailable,
                    .code_ = "blocking_pool_unavailable",
                    .message_ = error.what()})};
    } catch (...) {
        // An unexpected exception (e.g. a database/library error) may carry
        // internal detail -- table names, query fragments, file paths. Do NOT echo
        // what() to the client: an info without a message renders the generic
        // "Internal Server Error" body. The exception_ptr is still set on the context, so an on_error
        // handler can log or inspect the full detail server-side; without one it
        // is reported to stderr (report_unexpected_failure).
        return {.error_ = http_error({.status_ = ruvia::http_status::internal_server_error}),
            .unexpected_ = true};
    }
}

// An unexpected exception answered without an application error handler would
// otherwise leave no server-side trace at all.
void report_unexpected_failure(const classified_error& error_info, const std::exception_ptr& exception,
    detail::http_error_handler_ref error_handler) noexcept {
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

detail::default_exception_response detail::make_default_exception_response(
    const std::exception_ptr& exception) {
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
        auto response = co_await run_unmatched_chain(
            context_value, callback_access::bind<task<http_response>(context&)>(terminal));
        co_return response;
    } catch (...) {
        exception = std::current_exception();
    }
    co_return co_await finalize_failure(context_value, exception);
}

task<http_response> detail::route_table::handle_exception(const http_request& request, std::exception_ptr exception, const context_services& services) const {
    return handle_exception(request, std::move(exception), services,
        services.request_stream() ? nullptr : error_handler_for(request.path()));
}

task<http_response> detail::route_table::handle_exception(const http_request& request, std::exception_ptr exception, const context_services& services,
    http_error_handler_ref error_handler) const {
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

task<http_response> detail::route_table::handle_not_found(
    const http_request& request, const context_services& services) const {
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
        co_return co_await run_unmatched_chain(
            context_value, callback_access::bind<task<http_response>(context&)>(terminal));
    } catch (...) {
        exception = std::current_exception();
    }
    co_return co_await finalize_failure(context_value, exception);
}

task<http_response> detail::route_table::handle_exception(
    context& context_value, std::exception_ptr exception) const {
    return handle_exception(context_value, std::move(exception),
        error_handler_for(detail::context_access::request(context_value).path()));
}

task<http_response> detail::route_table::handle_exception(context& context_value,
    std::exception_ptr exception, http_error_handler_ref error_handler) const {
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
[[nodiscard]] auto select_prefix_handler(
    const std::vector<stored_type>& stored, std::string_view path) noexcept {
    for (const auto& candidate : stored) {
        const std::string_view prefix(candidate.prefix_);
        if (detail::path_is_under_prefix(path, prefix)) {
            return candidate.handler_;
        }
    }
    return decltype(stored.front().handler_){nullptr};
}

}  // namespace

detail::http_error_handler_ref detail::route_table::error_handler_for(
    std::string_view path) const noexcept {
    const auto handler = select_prefix_handler(plan_->prefix_error_handlers_, path);
    return handler != nullptr ? handler : plan_->error_handler_;
}

detail::http_not_found_handler_ref detail::route_table::not_found_handler_for(
    std::string_view path) const noexcept {
    const auto handler = select_prefix_handler(plan_->prefix_not_found_handlers_, path);
    return handler != nullptr ? handler : plan_->not_found_handler_;
}

}  // namespace ruvia

namespace ruvia {

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
    detail::context_access::set_response(
        context_value, detail::make_default_error_response(http_error_info({.status_ = ruvia::http_status::internal_server_error,
                           .code_ = code,
                           .message_ = message})));
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
    explicit next_control_scope(detail::next_state::control_type& control) noexcept
        : control_(&control) {}

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
    static constexpr std::size_t next_offset =
        (sizeof(detail::next_state::control_type) + alignof(next) - 1) / alignof(next) * alignof(next);
    static constexpr std::size_t size = next_offset + sizeof(next);
    static constexpr std::size_t alignment = alignof(next) > alignof(detail::next_state::control_type)
                                                 ? alignof(next)
                                                 : alignof(detail::next_state::control_type);
};

task<void> run_chain_hop(detail::next_state state_value);

[[nodiscard]] std::size_t applicable_frame(
    const detail::middleware_chain& chain, std::size_t index, const context& context_value) noexcept {
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
            auto* storage = static_cast<std::byte*>(
                detail::context_access::allocate_request_storage(context_value, next_hop_layout::size, next_hop_layout::alignment));
            auto& control = *std::construct_at(reinterpret_cast<detail::next_state::control_type*>(storage));
            const next_control_scope control_scope(control);
            auto& next_value = detail::next_access::make_at(storage + next_hop_layout::next_offset,
                detail::next_state{
                    .chain_ = &chain, .context_ = &context_value, .control_ = &control, .index_ = index + 1},
                &run_chain_hop);
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
    state_value.invocation_ =
        control == nullptr ? detail::next_state::invocation_type::expired : control->begin_invocation();
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
    return middleware_chain{.table_ = this,
        .frames_ = std::span<const route_middleware>(middleware_frames_).subspan(offset, count),
        .scopes_ = route.has_conditional_middleware()
                       ? std::span<const std::string_view>(plan_->middleware_scopes_).subspan(offset, count)
                       : std::span<const std::string_view>{},
        .terminal_ = {},
        .failure_ = &route_table::contain_failure};
}

namespace {

task<http_response> mark_explicit_head_route_response(task<http_response> operation) {
    auto response = co_await std::move(operation);
    if (!response.has_file_representation() && response.body_bytes().empty() &&
        !response.status().is_redirection() && !response.status().is_error()) {
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
    auto operation = !route.has_middleware()
                         ? resolved.handler()(context_value)
                         : respond_through_chain(route_chain(route), context_value, resolved.handler());
    if (route.method() == http_known_method::head) {
        return mark_explicit_head_route_response(std::move(operation));
    }
    return operation;
}

task<http_response> detail::route_table::respond_through_chain(
    middleware_chain chain, context& context_value, route_handler terminal) const {
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

task<http_response> detail::route_table::run_unmatched_chain(
    context& context_value, route_handler terminal) const {
    return respond_through_chain(
        middleware_chain{.table_ = this,
            .frames_ = std::span<const route_middleware>(middleware_frames_)
                .subspan(plan_->unmatched_middleware_offset_, plan_->unmatched_middleware_count_),
            .scopes_ = {},
            .terminal_ = {},
            .failure_ = &route_table::contain_failure},
        context_value, terminal);
}

task<void> detail::route_table::contain_failure(
    const route_table& table, context& context_value, std::exception_ptr exception) {
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

}  // namespace ruvia

// Running a route that streams its response: binding the writer to the context
// for the handler's lifetime, producing the head the HTTP/1 sink commits, and
// unwinding when the handler throws after bytes are already on the wire.

namespace ruvia {

namespace {

class response_stream_context_binding final {
public:
    explicit response_stream_context_binding(response_stream_writer* writer) noexcept
        : writer_(writer) {}

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

task<std::optional<http_response>> detail::route_table::dispatch_stream_route(
    const http_request& request, const resolved_route& resolved,
    route_stream_handler handler, const context_services& services,
    response_stream_writer* response_stream) const {
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

task<std::optional<http_response>> detail::route_table::dispatch_bound_stream(
    const resolved_route& resolved, context& context_value, route_stream_handler handler) const {
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
    routed_body(body_reader* source, std::optional<std::size_t> limit, inbound_buffer_account* inbound)
        : source_(source),
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
    explicit router_state(worker_ref worker)
        : worker_(std::move(worker)) {
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
        registration(std::string_view method, std::string_view path, handler_type handler,
            std::span<const controller_middleware_descriptor> modifiers, upgrade_type upgrade = {})
            : method_(method),
              path_(path),
              handler_(std::move(handler)),
              upgrade_(std::move(upgrade)),
              modifiers_(modifiers.begin(), modifiers.end()) {
            if (!is_valid_http_method_token(method)) {
                throw std::invalid_argument("route method must be a valid HTTP token");
            }
            if (!std::visit([](const auto& value) { return static_cast<bool>(value); }, handler_)) {
                throw std::invalid_argument("route handler must not be empty");
            }
        }
    };

    [[nodiscard]] static route_endpoint make_endpoint(const registration& route, route_stream_handler::invoke_type invoke,
        request_body_mode body_mode) {
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
                const auto body_mode = std::ranges::any_of(route.modifiers_, [](const auto& value) { return value.stream_request_body(); })
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
                },
                    route.handler_);
            }
            configuration_.apply(builder);
            auto plan = builder.compile();
            auto table = std::make_unique<route_table>(*plan, std::span<void* const>(targets));
            const auto route_limits = table->has_route_rate_limit() ? route_rate_limit_presence::present
                                                                    : route_rate_limit_presence::absent;
            auto limiter = rate_limit_ || route_limits == route_rate_limit_presence::present
                               ? std::make_unique<rate_limiter>(rate_limit_ ? std::optional<rate_limit_rule>(rate_limit_->rule_) : std::nullopt, route_limits,
                                     rate_limit_ ? rate_limit_->capacity_per_worker_ : default_rate_limit_capacity_per_worker)
                               : std::unique_ptr<rate_limiter>{};
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
        if (stream.routed_ || stream.response_.committed() || context_access::output_committed(context_value) ||
            context_access::has_response(context_value)) {
            throw std::logic_error("request routing or response selection has already begun");
        }
        stream.routed_ = true;
        const auto& table = *self->table_;
        const auto& request = stream.request();
        const auto resolution = resolve_dispatched_request(table, request);
        const auto* resolved = resolution.resolved();
        context_access::route_binding binding(context_value,
            resolved ? resolved->route().path() : std::string_view{},
            resolved ? resolved->route().param_names() : std::span<const std::string_view>{},
            resolved ? resolved->match().values() : std::span<const std::string_view>{},
            resolved ? reinterpret_cast<std::uintptr_t>(&resolved->route()) : 0);
        auto& services = context_access::services(context_value);
        services = services.with_request_stream({});
        services.bind_routes(table);
        if (self->limiter_) {
            services.bind_route_rate_limiter(*self->limiter_);
        }
        services.bind_router_capabilities(self->clients_.empty() ? nullptr : &self->clients_,
            self->worker_states_ ? &*self->worker_states_ : nullptr);
        binding.bind_worker_states();
        // Only this router's own rule: the host's application-wide rule never
        // covers a request stream.
        if (const auto rejection = decide_request_rate_limit(
                self->limiter_.get(), context_value.conn().client().address(), services.pinned_arrival())) {
            auto response = co_await table.handle_error(request, rate_limit_rejection_error(), services);
            apply_rate_limit_rejection_headers(response, *rejection);
            context_access::set_response(context_value, std::move(response));
            co_return;
        }
        if (resolved == nullptr) {
            auto buffered_services = services;
            buffered_services.response_output() = {};
            http_response response;
            co_await table.dispatch_buffered_response(request, resolution, document_root_binding::none(),
                buffered_services, static_file_selection_mode::identity_only, response);
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
                           : route.max_request_body_bytes() != 0 ? std::optional<std::size_t>(route.max_request_body_bytes())
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
        const auto status = stream.request().known_method() == http_known_method::connect ? http_status::ok
                                                                                          : http_status::switching_protocols;
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
            auto response = co_await table.handle_error(request,
                websocket_route ? http_error_info({.status_ = http_status::bad_request,
                                      .code_ = "invalid_websocket_handshake",
                                      .message_ = "invalid WebSocket handshake"})
                                : http_error_info({.status_ = http_status::bad_request,
                                      .message_ = "tunnel route requires a CONNECT or upgrade request without content"}),
                services);
            context_access::set_response(context_value, std::move(response));
            co_return;
        }
        auto terminal = [&stream, &resolved](context& terminal_context) {
            return run_upgrade_terminal(stream, resolved, terminal_context);
        };
        std::exception_ptr failure;
        try {
            auto response = co_await table.dispatch_bound_stream(
                resolved, context_value, callback_access::bind<task<void>(context&)>(terminal));
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

::ruvia::router::router(worker_ref worker)
    : state_(std::make_shared<detail::router_state>(std::move(worker))) {}
::ruvia::router::~router() = default;

::ruvia::router& ::ruvia::router::add_buffered(std::string_view method, std::string_view path, router_handler handler,
    std::span<const detail::controller_middleware_descriptor> modifiers) {
    state_->require_editable();
    state_->registrations_.emplace_back(method, path, std::move(handler), modifiers);
    return *this;
}

::ruvia::router& ::ruvia::router::add_stream(std::string_view method, std::string_view path, router_stream_handler handler,
    std::span<const detail::controller_middleware_descriptor> modifiers) {
    state_->require_editable();
    state_->registrations_.emplace_back(method, path, std::move(handler), modifiers);
    return *this;
}

::ruvia::router& ::ruvia::router::add_websocket(std::string_view path, router_stream_handler handler, websocket_route_config config,
    std::span<const detail::controller_middleware_descriptor> modifiers) {
    state_->require_editable();
    state_->registrations_.emplace_back("GET", path, std::move(handler), modifiers, std::move(config));
    return *this;
}

::ruvia::router& ::ruvia::router::add_tunnel(std::string_view protocol, std::string_view target, router_stream_handler handler,
    http_tunnel_route_config config, std::span<const detail::controller_middleware_descriptor> modifiers) {
    state_->require_editable();
    state_->registrations_.emplace_back("CONNECT", target, std::move(handler), modifiers,
        detail::router_state::tunnel_upgrade{std::string(protocol), config});
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

}  // namespace ruvia

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
[[nodiscard]] std::vector<controller_middleware_descriptor> merge_middleware_descriptors(
    std::span<const controller_middleware_descriptor> base, std::span<const controller_middleware_descriptor> extra) {
    std::vector<controller_middleware_descriptor> middlewares;
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
    std::vector<controller_middleware_descriptor> middlewares)
    : controller_route_builder(
          router_value, join_controller_paths({}, prefix), std::move(middlewares), owned_prefix_tag_type{}) {}

detail::controller_route_builder::controller_route_builder(router& router_value, std::string prefix,
    std::vector<controller_middleware_descriptor> middlewares, owned_prefix_tag_type)
    : router_(router_value),
      prefix_(std::move(prefix)),
      middlewares_(std::move(middlewares)) {}

detail::controller_route_builder::~controller_route_builder() = default;

void detail::controller_route_builder::register_route(http_known_method method, std::string_view method_token,
    std::string_view path, route_handler handler, std::span<const controller_middleware_descriptor> middlewares) const {
    const auto body_mode = declared_request_body_mode(middlewares_, middlewares);
    const scoped_controller_path joined(prefix_, path);
    router_.register_endpoint(method, method_token, joined.view_,
        route_endpoint::buffered(callback_access::invoke(handler), body_mode), callback_access::target(handler),
        middlewares_, middlewares);
}

void detail::controller_route_builder::register_route(http_known_method method, std::string_view method_token,
    std::string_view path, route_stream_handler handler,
    std::span<const controller_middleware_descriptor> middlewares) const {
    const auto body_mode = declared_request_body_mode(middlewares_, middlewares);
    const scoped_controller_path joined(prefix_, path);
    router_.register_endpoint(method, method_token, joined.view_,
        route_endpoint::response_stream(callback_access::invoke(handler), body_mode),
        callback_access::target(handler), middlewares_, middlewares);
}

void detail::controller_route_builder::register_tunnel_route(std::string_view protocol,
    std::string_view target, route_stream_handler handler,
    std::span<const controller_middleware_descriptor> middlewares, http_tunnel_route_config config) const {
    std::optional<scoped_controller_path> joined;
    if (!protocol.empty()) {
        joined.emplace(prefix_, target);
    }
    const auto path = protocol.empty() ? target : joined->view_;
    router_.register_endpoint(http_known_method::connect, {}, path,
        route_endpoint::tunnel(callback_access::invoke(handler), protocol, config),
        callback_access::target(handler), middlewares_, middlewares);
}

void detail::controller_route_builder::register_websocket_route(http_known_method method,
    std::string_view path, route_stream_handler handler,
    std::span<const controller_middleware_descriptor> middlewares,
    websocket_route_config websocket_config) const {
    const scoped_controller_path joined(prefix_, path);
    router_.register_endpoint(method, {}, joined.view_,
        route_endpoint::websocket(callback_access::invoke(handler), std::move(websocket_config)),
        callback_access::target(handler), middlewares_, middlewares);
}
detail::controller_route_builder detail::controller_route_builder::create_scope(
    std::string_view prefix, std::span<const controller_middleware_descriptor> middlewares) const {
    auto merged = merge_middleware_descriptors(middlewares_, middlewares);
    return controller_route_builder(router_, join_controller_paths(prefix_, prefix),
        std::move(merged), owned_prefix_tag_type{});
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

namespace ruvia {
namespace detail {

void context_session_state::bind(const session_middleware* owner_value) {
    if (available_) {
        throw std::logic_error("session middleware is already bound");
    }
    require_mutable();
    owner_ = owner_value;
    available_ = true;
}

void context_session_state::require_mutable() const {
    if (phase_ != phase_type::active) {
        throw std::logic_error("session cannot change after response commit begins");
    }
}

bool context_session_state::begin_commit() {
    if (!available_ || phase_ == phase_type::committed) {
        return false;
    }
    if (phase_ == phase_type::failed) {
        std::rethrow_exception(failure_);
    }
    require_mutable();
    phase_ = phase_type::committing;
    return true;
}

void context_session_state::finish_commit() noexcept {
    phase_ = phase_type::committed;
}

void context_session_state::fail_commit(std::exception_ptr exception) noexcept {
    failure_ = std::move(exception);
    phase_ = phase_type::failed;
}

void context_session_state::observe_presented_id(std::string_view id) {
    require_mutable();
    value_.template emplace<session_unrecognized>(std::string(id));
}

void context_session_state::load_recognized(std::string_view data) {
    require_mutable();
    auto* presented = std::get_if<session_unrecognized>(&value_);
    if (presented == nullptr) {
        throw std::logic_error("recognized session requires a presented id");
    }
    auto data_copy = std::string(data);
    auto id = std::move(presented->id_);
    value_.template emplace<session_loaded>(std::move(id), std::move(data_copy));
}

void context_session_state::set(std::string_view data) {
    require_mutable();
    if (data.empty()) {
        clear();
        return;
    }
    if (auto* loaded_state = std::get_if<session_loaded>(&value_)) {
        auto data_copy = std::string(data);
        auto id = std::move(loaded_state->id_);
        value_.template emplace<session_rotate>(std::move(id), std::move(data_copy));
        return;
    }
    if (auto* rotated = std::get_if<session_rotate>(&value_)) {
        rotated->data_.assign(data);
        return;
    }
    if (auto* fresh = std::get_if<session_persist_new>(&value_)) {
        fresh->data_.assign(data);
        return;
    }
    if (auto* cleared = std::get_if<session_clear>(&value_)) {
        if (!cleared->old_id_.has_value()) {
            value_.template emplace<session_persist_new>(std::string(data));
            return;
        }
        auto data_copy = std::string(data);
        auto old_id = std::move(*cleared->old_id_);
        value_.template emplace<session_rotate>(std::move(old_id), std::move(data_copy));
        return;
    }
    value_.template emplace<session_persist_new>(std::string(data));
}

void context_session_state::clear() {
    require_mutable();
    if (std::get_if<session_clear>(&value_) != nullptr) {
        return;
    }
    if (auto* loaded_state = std::get_if<session_loaded>(&value_)) {
        std::optional<std::string> id(std::move(loaded_state->id_));
        value_.template emplace<session_clear>(std::move(id));
        return;
    }
    if (auto* rotated = std::get_if<session_rotate>(&value_)) {
        std::optional<std::string> id(std::move(rotated->old_id_));
        value_.template emplace<session_clear>(std::move(id));
        return;
    }
    value_.template emplace<session_clear>(std::nullopt);
}

void context_session_state::regenerate() {
    require_mutable();
    auto* loaded_state = std::get_if<session_loaded>(&value_);
    if (loaded_state == nullptr) {
        return;
    }
    auto old_id = std::move(loaded_state->id_);
    auto data = std::move(loaded_state->data_);
    if (data.empty()) {
        value_.template emplace<session_clear>(
            std::optional<std::string>(std::move(old_id)));
    } else {
        value_.template emplace<session_rotate>(std::move(old_id), std::move(data));
    }
}

std::string_view context_session_state::data() const& noexcept {
    if (const auto* state = std::get_if<session_loaded>(&value_)) {
        return state->data_;
    }
    if (const auto* state = std::get_if<session_persist_new>(&value_)) {
        return state->data_;
    }
    if (const auto* state = std::get_if<session_rotate>(&value_)) {
        return state->data_;
    }
    return {};
}

}  // namespace detail

std::string_view session::data() const& noexcept {
    return state_->data();
}

void session::set(std::string_view data) {
    state_->set(data);
}

void session::clear() {
    state_->clear();
}

void session::regenerate() {
    state_->regenerate();
}

session context::session() {
    if (!session_state().available()) {
        throw std::logic_error("session capability is not bound for this request");
    }
    return ruvia::session(session_state());
}

#ifdef RUVIA_ENABLE_REDIS
task<void> detail::session_access::commit(context& context_value) {
    auto& state_value = context_value.session_state();
    if (!state_value.begin_commit()) {
        co_return;
    }
    try {
        if (const auto* owner = state_value.owner()) {
            co_await owner->commit(context_value);
        }
        state_value.finish_commit();
    } catch (...) {
        state_value.fail_commit(std::current_exception());
        throw;
    }
}
#endif

}  // namespace ruvia

#ifdef RUVIA_ENABLE_REDIS

namespace ruvia {
namespace {

// "<key prefix><session id>", inline for typical key sizes.
using session_key = detail::inline_text_concat<128>;

}  // namespace

const session_config& session_middleware::validate(const session_config& source_value) {
    if (source_value.redis_alias_.empty()) {
        throw std::invalid_argument("session Redis alias must not be empty");
    }
    if (!is_valid_http_header_name(source_value.cookie_name_)) {
        throw std::invalid_argument("session cookie name must be a valid HTTP token");
    }
    if (source_value.key_prefix_.empty()) {
        throw std::invalid_argument("session key prefix must not be empty");
    }
    if (source_value.ttl_.count() <= 0) {
        throw std::invalid_argument("session TTL must be greater than zero");
    }
    // New sessions expire through SET PX in milliseconds; rotation sends the
    // same TTL as EX seconds, which Redis likewise converts to milliseconds.
    if (source_value.ttl_ > std::chrono::duration_cast<std::chrono::seconds>(std::chrono::milliseconds::max())) {
        throw std::invalid_argument("session TTL must be representable in milliseconds");
    }
    return source_value;
}

session_middleware::session_middleware()
    : session_middleware(session_config{}) {}

session_middleware::session_middleware(const session_config& config)
    : redis_alias_(validate(config).redis_alias_),
      cookie_name_(config.cookie_name_),
      key_prefix_(config.key_prefix_),
      ttl_(config.ttl_) {}

task<void> session_middleware::invoke(context& c, next& next_value) {
    detail::session_access::bind(c, this);
    const auto cookie = c.req().cookie(cookie_name_);
    if (cookie && detail::is_valid_session_id(*cookie)) {
        detail::session_access::observe_presented_id(c, *cookie);
        auto get_task = [&] {
            const session_key key({key_prefix_, *cookie});
            return c.redis(redis_alias_).get(key.view());
        }();
        if (auto stored = co_await std::move(get_task)) {
            detail::session_access::load(c, *stored);
        }
    }

    co_await next_value();
    co_await detail::session_access::commit(c);
}

task<void> session_middleware::commit(context& c) const {
    const auto& state_value = detail::session_access::state(c);
    if (state_value.untouched() != nullptr || state_value.unrecognized() != nullptr ||
        state_value.loaded() != nullptr) {
        co_return;
    }

    const auto connection = c.conn();
    const bool secure = connection.scheme() == http_scheme::https;
    // Session Cookie 属于明确的跨替换响应策略，只在最终交付/流提交时应用。
    // Redis 成功后通过无分配移动发布完整字段，既有 commit guard 保证只持久化一次。
    auto& response = detail::context_access::response_policy(c);
    auto staged = response.headers().clone(1);
    const auto publish = [&]() noexcept {
        response.commit_headers(std::move(staged));
    };
    if (const auto* cleared = state_value.cleared()) {
        detail::append_expired_session_cookie_header(staged, cookie_name_, secure);
        if (cleared->old_id_.has_value()) {
            auto del_task = [&] {
                const session_key key({key_prefix_, *cleared->old_id_});
                return c.redis(redis_alias_).del(key.view());
            }();
            (void)(co_await std::move(del_task));
        }
        publish();
        co_return;
    }

    std::string_view data;
    std::string_view old_id;
    if (const auto* rotated = state_value.rotate()) {
        data = rotated->data_;
        old_id = rotated->old_id_;
    } else {
        data = state_value.persist_new()->data_;
    }

    std::array<char, detail::secure_token_size> id_buffer;
    const auto new_id = detail::generate_secure_token(id_buffer);
    detail::append_session_cookie_header(staged, cookie_name_, new_id, secure);

    const session_key key({key_prefix_, new_id});
    bool applied = false;
    if (!old_id.empty()) {
        auto eval_task = [&] {
            const session_key old_key({key_prefix_, old_id});
            // session_middleware::validate requires a positive TTL.
            std::array<char, http_max_unsigned_decimal_size> ttl_buffer;
            const auto ttl = http_format_unsigned(ttl_buffer, static_cast<std::uint64_t>(ttl_.count()));
            const std::array<std::string_view, 2> keys{old_key.view(), key.view()};
            const std::array<std::string_view, 2> args{data, ttl};
            // Redis executes the entire transition atomically. A stale request may
            // neither recreate a revoked identifier nor mint a successor from it.
            return c.redis(redis_alias_).eval(detail::session_rotation_script, keys, args);
        }();
        const auto result_value = co_await std::move(eval_task);
        if (result_value.kind() == redis_value::kind_type::error) {
            throw redis_error(redis_error::code_type::command_error, result_value.error());
        }
        applied = result_value.integer() == 1;
    } else {
        auto set_task = [&] {
            redis_set_options options;
            options.condition_ = redis_set_condition::if_absent;
            options.expiration_ = redis_set_expiration::expires_after(ttl_);
            return c.redis(redis_alias_).set(key.view(), data, std::move(options));
        }();
        const auto result_value = co_await std::move(set_task);
        applied = result_value.applied();
    }
    if (!applied) {
        throw http_error({.status_ = http_status::conflict,
            .code_ = "session_conflict",
            .message_ = "session expired or was revoked; retry with a new session"});
    }
    publish();
}

}  // namespace ruvia

#endif  // RUVIA_ENABLE_REDIS

namespace ruvia::detail {

namespace {

constexpr std::uint64_t empty_hash = 0;
constexpr rate_limit_scope default_scope{.route_ = 1};
constexpr std::uintptr_t fallback_route_scope = 2;

[[nodiscard]] rate_limit_hash_key random_hash_key() {
    std::random_device source;
    rate_limit_hash_key key{};
    for (auto& word : key) {
        word = (std::uint64_t{source()} << 32) | source();
    }
    return key;
}

[[nodiscard]] std::uint64_t load_little_endian(const unsigned char* bytes, std::size_t size) noexcept {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < size; ++index) {
        value |= std::uint64_t{bytes[index]} << (8 * index);
    }
    return value;
}

void store_little_endian(unsigned char* bytes, std::uint64_t value) noexcept {
    for (std::size_t index = 0; index < 8; ++index) {
        bytes[index] = static_cast<unsigned char>(value >> (8 * index));
    }
}

struct siphash_state final {
    std::uint64_t v0_;
    std::uint64_t v1_;
    std::uint64_t v2_;
    std::uint64_t v3_;

    void round() noexcept {
        v0_ += v1_;
        v1_ = std::rotl(v1_, 13);
        v1_ ^= v0_;
        v0_ = std::rotl(v0_, 32);
        v2_ += v3_;
        v3_ = std::rotl(v3_, 16);
        v3_ ^= v2_;
        v0_ += v3_;
        v3_ = std::rotl(v3_, 21);
        v3_ ^= v0_;
        v2_ += v1_;
        v1_ = std::rotl(v1_, 17);
        v1_ ^= v2_;
        v2_ = std::rotl(v2_, 32);
    }

    void compress(std::uint64_t word) noexcept {
        v3_ ^= word;
        round();
        v0_ ^= word;
    }
};

// SipHash-1-3: the keyed PRF behind Rust's std hasher, the grade of keyed hash
// pingora-limits places its counters with.
[[nodiscard]] std::uint64_t siphash13(const rate_limit_hash_key& key, std::span<const unsigned char> message) noexcept {
    siphash_state state{
        .v0_ = key[0] ^ 0x736f6d6570736575ULL,
        .v1_ = key[1] ^ 0x646f72616e646f6dULL,
        .v2_ = key[0] ^ 0x6c7967656e657261ULL,
        .v3_ = key[1] ^ 0x7465646279746573ULL,
    };
    const auto whole_words = message.size() - message.size() % 8;
    for (std::size_t offset = 0; offset < whole_words; offset += 8) {
        state.compress(load_little_endian(message.data() + offset, 8));
    }
    state.compress((static_cast<std::uint64_t>(message.size()) << 56) |
                   load_little_endian(message.data() + whole_words, message.size() - whole_words));
    state.v2_ ^= 0xff;
    state.round();
    state.round();
    state.round();
    return state.v0_ ^ state.v1_ ^ state.v2_ ^ state.v3_;
}

// The caller bounds `key` by rate_limit_max_key_bytes.
[[nodiscard]] std::uint64_t key_hash(
    const rate_limit_hash_key& hash_key, rate_limit_scope scope, std::string_view key) noexcept {
    std::array<unsigned char, 16 + rate_limit_max_key_bytes> message{};
    store_little_endian(message.data(), static_cast<std::uint64_t>(scope.route_));
    store_little_endian(message.data() + 8, static_cast<std::uint64_t>(scope.rule_));
    if (!key.empty()) {
        std::memcpy(message.data() + 16, key.data(), key.size());
    }
    const auto hash = siphash13(hash_key, std::span(message).first(16 + key.size()));
    return hash == empty_hash ? 1 : hash;
}

[[nodiscard]] std::uint64_t saturating_add(std::uint64_t left, std::uint64_t right) noexcept {
    constexpr auto max_value = std::numeric_limits<std::uint64_t>::max();
    return right > max_value - left ? max_value : left + right;
}

// An unsigned 128-bit value, high word first so the defaulted ordering is the
// numeric one. The estimate's products need it: a count and a window length
// each span the full 64-bit range a rule allows.
struct wide_unsigned final {
    std::uint64_t high_{0};
    std::uint64_t low_{0};

    friend constexpr auto operator<=>(const wide_unsigned&, const wide_unsigned&) noexcept = default;
};

[[nodiscard]] constexpr wide_unsigned multiply_wide(std::uint64_t left, std::uint64_t right) noexcept {
    constexpr std::uint64_t half_mask = 0xffff'ffffULL;
    const auto low_low = (left & half_mask) * (right & half_mask);
    const auto low_high = (left & half_mask) * (right >> 32);
    const auto high_low = (left >> 32) * (right & half_mask);
    const auto high_high = (left >> 32) * (right >> 32);
    const auto middle = (low_low >> 32) + (low_high & half_mask) + (high_low & half_mask);
    return {.high_ = high_high + (low_high >> 32) + (high_low >> 32) + (middle >> 32),
        .low_ = (middle << 32) | (low_low & half_mask)};
}

// Requires a nonzero value.
[[nodiscard]] constexpr wide_unsigned wide_predecessor(wide_unsigned value) noexcept {
    return value.low_ == 0 ? wide_unsigned{.high_ = value.high_ - 1, .low_ = std::numeric_limits<std::uint64_t>::max()}
                           : wide_unsigned{.high_ = value.high_, .low_ = value.low_ - 1};
}

// floor(dividend / divisor) by restoring long division. Requires
// dividend.high_ < divisor, which is exactly when the quotient fits 64 bits.
[[nodiscard]] constexpr std::uint64_t divide_wide(wide_unsigned dividend, std::uint64_t divisor) noexcept {
    if (dividend.high_ == 0) {
        return dividend.low_ / divisor;
    }
    auto remainder = dividend.high_;
    std::uint64_t quotient = 0;
    for (int bit = 63; bit >= 0; --bit) {
        // A set top bit makes the shifted remainder 2^64 or more, above any
        // divisor; the wrapping subtraction below still yields the true value.
        const bool carry = (remainder >> 63) != 0;
        remainder = (remainder << 1) | ((dividend.low_ >> bit) & 1U);
        quotient <<= 1;
        if (carry || remainder >= divisor) {
            remainder -= divisor;
            quotient |= 1U;
        }
    }
    return quotient;
}

// Configuration validation and rate_limit's static_assert keep every window
// positive.
[[nodiscard]] std::uint64_t window_length_ms(const rate_limit_rule& rule) noexcept {
    return static_cast<std::uint64_t>(rule.window_.count());
}

[[nodiscard]] std::uint64_t elapsed_ms(std::chrono::steady_clock::time_point start,
    std::chrono::steady_clock::time_point now) noexcept {
    return now <= start ? 0 : static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count());
}

[[nodiscard]] bool key_equals(
    const rate_limit_slot& slot, rate_limit_scope scope, std::string_view key, std::uint64_t hash) noexcept {
    return slot.key_hash_ == hash && slot.scope_ == scope && slot.key_size_ == key.size() &&
           std::ranges::equal(key, std::span(slot.key_).first(slot.key_size_));
}

[[nodiscard]] bool stale(const rate_limit_slot& slot, std::chrono::steady_clock::time_point now) noexcept {
    return slot.key_hash_ != empty_hash && elapsed_ms(slot.window_start_, now) >= slot.stale_after_ms_;
}

// Moves the key to the window containing now. After one full window the
// current count becomes the previous one; after two or more neither count is
// inside the trailing window any more, so the key restarts as a new one.
void roll_window(rate_limit_slot& slot, const rate_limit_rule& rule, std::chrono::steady_clock::time_point now) noexcept {
    const auto window_ms = window_length_ms(rule);
    const auto elapsed = elapsed_ms(slot.window_start_, now);
    if (elapsed < window_ms) {
        return;
    }
    if (elapsed - window_ms < window_ms) {
        slot.previous_count_ = slot.current_count_;
        // The elapsed check guarantees this advance fits before now.
        slot.window_start_ += saturating_duration_cast(rule.window_);
    } else {
        slot.previous_count_ = 0;
        slot.window_start_ = now;
    }
    slot.current_count_ = 0;
}

// Refusals leave the counts alone, so the estimate only decays: the previous
// count fades out across the current window, and at the boundary the current
// count becomes the previous one. Retry-After is the first millisecond at
// which the admission test passes again; the reset is when the estimate
// reaches zero.
[[nodiscard]] rate_limit_rejection refusal(const rate_limit_rule& rule, std::uint64_t previous,
    std::uint64_t current, std::uint64_t window_ms, std::uint64_t remaining_ms) noexcept {
    const auto limit = static_cast<std::uint64_t>(rule.max_requests_);
    std::uint64_t retry_after_ms = 0;
    if (current >= limit) {
        // The next window opens with previous == limit and passes the test
        // once one millisecond of it has elapsed.
        retry_after_ms = saturating_add(remaining_ms, 1);
    } else {
        // Refused with room left means previous * remaining >= room * window,
        // so previous > 0 and the largest passing remaining,
        // floor((room * window - 1) / previous), lies below remaining.
        const auto passing_remaining_ms =
            divide_wide(wide_predecessor(multiply_wide(limit - current, window_ms)), previous);
        retry_after_ms = remaining_ms - passing_remaining_ms;
    }
    // Windows are below 2^63 ms, so this sum and the retry sum above stay exact.
    const auto reset_after_ms = current == 0 ? remaining_ms : saturating_add(remaining_ms, window_ms);
    return rate_limit_rejection(retry_after_ms, reset_after_ms, rule.max_requests_);
}

// Admits while previous * remaining / window + current < max_requests_, where
// remaining is the part of the current window still ahead, i.e. the share of
// the previous window that the trailing window still covers. Both sides are
// scaled by the window so the comparison is exact.
[[nodiscard]] std::optional<rate_limit_rejection> consume(
    rate_limit_slot& slot, const rate_limit_rule& rule, std::chrono::steady_clock::time_point now) noexcept {
    const auto window_ms = window_length_ms(rule);
    roll_window(slot, rule, now);
    const auto remaining_ms = window_ms - elapsed_ms(slot.window_start_, now);
    const auto limit = static_cast<std::uint64_t>(rule.max_requests_);
    const auto previous = static_cast<std::uint64_t>(slot.previous_count_);
    const auto current = static_cast<std::uint64_t>(slot.current_count_);
    if (current < limit && multiply_wide(previous, remaining_ms) < multiply_wide(limit - current, window_ms)) {
        ++slot.current_count_;
        return std::nullopt;
    }
    return refusal(rule, previous, current, window_ms, remaining_ms);
}

[[nodiscard]] std::optional<rate_limit_rejection> consume_new_key(rate_limit_slot& slot, rate_limit_scope scope,
    std::string_view key, std::uint64_t hash, const rate_limit_rule& rule, std::chrono::steady_clock::time_point now) noexcept {
    slot.scope_ = scope;
    slot.key_hash_ = hash;
    slot.window_start_ = now;
    const auto window_ms = window_length_ms(rule);
    slot.stale_after_ms_ = saturating_add(window_ms, window_ms);
    slot.current_count_ = 0;
    slot.previous_count_ = 0;
    slot.key_size_ = static_cast<std::uint8_t>(key.size());
    std::ranges::copy(key, slot.key_.begin());
    return consume(slot, rule, now);
}

[[nodiscard]] std::optional<rate_limit_rejection> overflow_decision(const rate_limit_rule& rule) noexcept {
    return rule.overflow_policy_ == rate_limit_overflow_policy::allow
               ? std::nullopt
               : std::optional(rate_limit_rejection(1, 1, rule.max_requests_));
}

[[nodiscard]] rate_limit_decision consume_decision(
    rate_limit_slot& slot, const rate_limit_rule& rule, std::chrono::steady_clock::time_point now) noexcept {
    const auto window_ms = window_length_ms(rule);
    roll_window(slot, rule, now);
    const auto remaining_ms = window_ms - elapsed_ms(slot.window_start_, now);
    const auto limit = static_cast<std::uint64_t>(rule.max_requests_);
    const auto previous = static_cast<std::uint64_t>(slot.previous_count_);
    const auto current = static_cast<std::uint64_t>(slot.current_count_);
    const auto prev_weighted = multiply_wide(previous, remaining_ms);
    if (current < limit && prev_weighted < multiply_wide(limit - current, window_ms)) {
        ++slot.current_count_;
        const auto next_current = current + 1U;
        const auto room_after_scaled = multiply_wide(limit - next_current, window_ms);
        std::size_t remaining_requests = 0;
        if (prev_weighted <= room_after_scaled) {
            const std::uint64_t borrow = room_after_scaled.low_ < prev_weighted.low_ ? 1U : 0U;
            const wide_unsigned diff{
                .high_ = room_after_scaled.high_ - prev_weighted.high_ - borrow,
                .low_ = room_after_scaled.low_ - prev_weighted.low_,
            };
            remaining_requests = static_cast<std::size_t>(divide_wide(diff, window_ms));
        }
        return rate_limit_decision{
            .allowed_ = true,
            .limit_ = rule.max_requests_,
            .remaining_ = remaining_requests,
            .retry_after_ms_ = 0,
            .reset_after_ms_ = saturating_add(remaining_ms, window_ms),
        };
    }
    const auto rejected = refusal(rule, previous, current, window_ms, remaining_ms);
    return rate_limit_decision{
        .allowed_ = false,
        .limit_ = rejected.max_requests(),
        .remaining_ = 0,
        .retry_after_ms_ = rejected.retry_after_ms(),
        .reset_after_ms_ = rejected.reset_after_ms(),
    };
}

[[nodiscard]] rate_limit_decision consume_new_key_decision(rate_limit_slot& slot,
    rate_limit_scope scope, std::string_view key, std::uint64_t hash,
    const rate_limit_rule& rule, std::chrono::steady_clock::time_point now) noexcept {
    slot.scope_ = scope;
    slot.key_hash_ = hash;
    slot.window_start_ = now;
    const auto window_ms = window_length_ms(rule);
    slot.stale_after_ms_ = saturating_add(window_ms, window_ms);
    slot.current_count_ = 0;
    slot.previous_count_ = 0;
    slot.key_size_ = static_cast<std::uint8_t>(key.size());
    std::ranges::copy(key, slot.key_.begin());
    return consume_decision(slot, rule, now);
}

[[nodiscard]] rate_limit_decision overflow_rate_limit_decision(const rate_limit_rule& rule) noexcept {
    if (rule.overflow_policy_ == rate_limit_overflow_policy::allow) {
        return rate_limit_decision{
            .allowed_ = true,
            .limit_ = rule.max_requests_,
            .remaining_ = 0,
            .retry_after_ms_ = 0,
            .reset_after_ms_ = window_length_ms(rule),
        };
    }
    return rate_limit_decision{
        .allowed_ = false,
        .limit_ = rule.max_requests_,
        .remaining_ = 0,
        .retry_after_ms_ = 1,
        .reset_after_ms_ = 1,
    };
}
}  // namespace

void validate_rate_limit_config(const rate_limit_config& config) {
    const auto& rule = config.rule_;
    if (rule.max_requests_ == 0) {
        throw std::invalid_argument("rate limit max requests must be greater than zero");
    }
    if (rule.window_.count() <= 0) {
        throw std::invalid_argument("rate limit window must be greater than zero");
    }
    if (rule.overflow_policy_ != rate_limit_overflow_policy::deny &&
        rule.overflow_policy_ != rate_limit_overflow_policy::allow) {
        throw std::invalid_argument("rate limit overflow policy is invalid");
    }
    if (!std::has_single_bit(config.capacity_per_worker_)) {
        throw std::invalid_argument(
            "rate-limit capacity per worker must be a power of two");
    }
}

rate_limiter::rate_limiter(std::optional<rate_limit_rule> default_rule_per_worker,
    route_rate_limit_presence route_rules, std::size_t capacity_value)
    : default_rule_per_worker_(default_rule_per_worker) {
    // http_server_options::set_rate_limit, the only writer of the rule and the
    // capacity, validated both; the capacity is a power of two for the mask.
    if (default_rule_per_worker_.has_value() || route_rules == route_rate_limit_presence::present) {
        slots_.resize(capacity_value);
        hash_key_ = random_hash_key();
    }
}

void rate_limiter::adopt_route_storage(rate_limiter& prepared) noexcept {
    if (slots_.empty()) {
        slots_.swap(prepared.slots_);
        std::swap(hash_key_, prepared.hash_key_);
    }
}

std::optional<rate_limit_rejection> rate_limiter::allow_default(
    std::string_view remote_address, std::chrono::steady_clock::time_point now) noexcept {
    return default_rule_per_worker_.has_value()
               ? allow(default_scope, remote_address, *default_rule_per_worker_, now)
               : std::nullopt;
}

std::optional<rate_limit_rejection> rate_limiter::allow_route(std::uintptr_t route_scope,
    const void* rule_declaration, std::string_view remote_address, const rate_limit_rule& rule,
    std::chrono::steady_clock::time_point now) noexcept {
    return allow({.route_ = route_scope == 0 ? fallback_route_scope : route_scope,
                     .rule_ = reinterpret_cast<std::uintptr_t>(rule_declaration)},
        remote_address, rule, now);
}

std::optional<rate_limit_rejection> rate_limiter::allow(rate_limit_scope scope, std::string_view key,
    const rate_limit_rule& rule, std::chrono::steady_clock::time_point now) noexcept {
    if (slots_.empty() || key.size() > rate_limit_max_key_bytes) {
        return overflow_decision(rule);
    }

    // Preserve the arrival's sub-millisecond phase; only elapsed time is quantized.
    now = std::max(now, std::chrono::steady_clock::time_point{});
    const auto hash = key_hash(hash_key_, scope, key);
    const auto mask = slots_.size() - 1;
    const auto start = static_cast<std::size_t>(hash) & mask;
    rate_limit_slot* reclaimable = nullptr;

    // Slots never return to empty, so a key is installed and later found
    // within the same bounded probe window from its home slot. A saturated
    // probe window costs a fixed probe count instead of a scan of the whole
    // table, like a pingora-limits estimator's fixed per-operation cost.
    const auto probe_window = std::min(slots_.size(), rate_limit_probe_window);
    for (std::size_t probe_value = 0; probe_value < probe_window; ++probe_value) {
        auto& slot = slots_[(start + probe_value) & mask];
        if (slot.key_hash_ == empty_hash) {
            return consume_new_key(reclaimable == nullptr ? slot : *reclaimable, scope, key, hash, rule, now);
        }
        if (key_equals(slot, scope, key, hash)) {
            return consume(slot, rule, now);
        }
        if (reclaimable == nullptr && stale(slot, now)) {
            reclaimable = &slot;
        }
    }

    if (reclaimable != nullptr) {
        return consume_new_key(*reclaimable, scope, key, hash, rule, now);
    }
    return overflow_decision(rule);
}

rate_limit_decision rate_limiter::decide_default(
    std::string_view key, std::chrono::steady_clock::time_point now) noexcept {
    if (!default_rule_per_worker_.has_value()) {
        return rate_limit_decision{};
    }
    return decide_rule(default_scope, key, *default_rule_per_worker_, now);
}

rate_limit_decision rate_limiter::decide_rule(rate_limit_scope scope, std::string_view key,
    const rate_limit_rule& rule, std::chrono::steady_clock::time_point now) noexcept {
    if (slots_.empty() || key.size() > rate_limit_max_key_bytes) {
        return overflow_rate_limit_decision(rule);
    }
    now = std::max(now, std::chrono::steady_clock::time_point{});
    const auto hash = key_hash(hash_key_, scope, key);
    const auto mask = slots_.size() - 1;
    const auto start = static_cast<std::size_t>(hash) & mask;
    rate_limit_slot* reclaimable = nullptr;
    const auto probe_window = std::min(slots_.size(), rate_limit_probe_window);
    for (std::size_t probe_value = 0; probe_value < probe_window; ++probe_value) {
        auto& slot = slots_[(start + probe_value) & mask];
        if (slot.key_hash_ == empty_hash) {
            return consume_new_key_decision(
                reclaimable == nullptr ? slot : *reclaimable, scope, key, hash, rule, now);
        }
        if (key_equals(slot, scope, key, hash)) {
            return consume_decision(slot, rule, now);
        }
        if (reclaimable == nullptr && stale(slot, now)) {
            reclaimable = &slot;
        }
    }
    if (reclaimable != nullptr) {
        return consume_new_key_decision(*reclaimable, scope, key, hash, rule, now);
    }
    return overflow_rate_limit_decision(rule);
}

namespace {

[[nodiscard]] std::optional<rate_limit_rejection> decide_route_rate_limit(
    context& context_value, const void* declaration, const rate_limit_rule& rule) noexcept {
    auto* limiter = context_access::rate_limiter(context_value);
    if (limiter == nullptr) {
        return std::nullopt;
    }

    char key_buffer[rate_limit_key_buffer_bytes];
    // The client, not the hop -- same as the app-wide limiter. Behind a trusted
    // proxy, remote() is the proxy and every caller would share one key.
    return limiter->allow_route(context_access::route_rate_limit_scope(context_value), declaration,
        rate_limit_key_for(context_value.conn().client().address(), key_buffer), rule,
        rate_limit_instant(context_access::rate_limit_arrival(context_value)));
}

void set_unsigned_header(http_response& response, std::string_view name, std::uint64_t value) {
    std::array<char, http_max_unsigned_decimal_size> buffer;
    response.header(name, http_format_unsigned(buffer, value));
}

// Rounds up in the unsigned millisecond domain, where every delay the limiter
// reports fits: a client waiting the whole seconds named must not arrive
// before the millisecond the limiter computed.
[[nodiscard]] std::uint64_t whole_seconds_after(std::uint64_t delay_ms) noexcept {
    return delay_ms / 1000 + (delay_ms % 1000 == 0 ? 0 : 1);
}

}  // namespace

http_error_info rate_limit_rejection_error() noexcept {
    return http_error_info({.status_ = ruvia::http_status::too_many_requests,
        .code_ = "too_many_requests",
        .message_ = "rate limit exceeded"});
}

// Remaining is 0 because the refusal itself shows the estimate has no room.
// Retry-After names when the estimate admits the next request; Reset names
// when it has drained to zero and the whole limit is available again.
void apply_rate_limit_rejection_headers(http_response& response, const rate_limit_rejection& rejection) {
    response.reserve_headers(response.headers().size() + 4U);
    set_unsigned_header(response, "Retry-After", whole_seconds_after(rejection.retry_after_ms()));
    set_unsigned_header(response, "X-RateLimit-Limit", rejection.max_requests());
    response.header_stable_view("X-RateLimit-Remaining", "0");
    set_unsigned_header(response, "X-RateLimit-Reset", whole_seconds_after(rejection.reset_after_ms()));
}

void enforce_route_rate_limit(context& context_value, const void* declaration, const rate_limit_rule& rule) {
    if (const auto rejection = decide_route_rate_limit(context_value, declaration, rule)) {
        throw rate_limit_rejected(*rejection);
    }
}

}  // namespace ruvia::detail

namespace ruvia {

std::string_view format_rate_limit_address_key(
    const ip_address& address, std::span<char, rate_limit_key_buffer_bytes> buffer) noexcept {
    return detail::rate_limit_key_for(address, buffer);
}

std::string_view format_rate_limit_address_key(
    std::string_view address, std::span<char, rate_limit_key_buffer_bytes> buffer) noexcept {
    return detail::rate_limit_key_for(address, buffer);
}

void apply_rate_limit_headers(http_response& response, const rate_limit_decision& decision) {
    response.reserve_headers(response.headers().size() + (decision.allowed() ? 3U : 4U));
    if (!decision.allowed()) {
        detail::set_unsigned_header(response, "Retry-After", decision.retry_after_seconds());
    }
    detail::set_unsigned_header(response, "X-RateLimit-Limit", decision.limit());
    detail::set_unsigned_header(response, "X-RateLimit-Remaining", decision.remaining());
    detail::set_unsigned_header(response, "X-RateLimit-Reset", decision.reset_after_seconds());
}

struct sliding_window_rate_limiter::impl_type final {
    impl_type(std::optional<rate_limit_rule> default_rule, std::size_t capacity_value)
        : limiter_(default_rule, detail::route_rate_limit_presence::present, capacity_value) {}

    detail::rate_limiter limiter_;
};

sliding_window_rate_limiter::sliding_window_rate_limiter(const rate_limit_config& config) {
    detail::validate_rate_limit_config(config);
    impl_ = std::make_unique<impl_type>(config.rule_, config.capacity_per_worker_);
}

sliding_window_rate_limiter::sliding_window_rate_limiter(std::size_t capacity_per_worker) {
    if (!std::has_single_bit(capacity_per_worker)) {
        throw std::invalid_argument("rate-limit capacity per worker must be a power of two");
    }
    impl_ = std::make_unique<impl_type>(std::nullopt, capacity_per_worker);
}
sliding_window_rate_limiter::sliding_window_rate_limiter(
    sliding_window_rate_limiter&&) noexcept = default;
sliding_window_rate_limiter& sliding_window_rate_limiter::operator=(
    sliding_window_rate_limiter&&) noexcept = default;
sliding_window_rate_limiter::~sliding_window_rate_limiter() = default;

bool sliding_window_rate_limiter::has_default_rule() const noexcept {
    return impl_ != nullptr && impl_->limiter_.has_default_rule();
}

std::size_t sliding_window_rate_limiter::capacity() const noexcept {
    return impl_ != nullptr ? impl_->limiter_.capacity() : 0;
}

rate_limit_decision sliding_window_rate_limiter::decide(
    std::string_view key, std::chrono::steady_clock::time_point now) noexcept {
    if (impl_ == nullptr) {
        return rate_limit_decision{};
    }
    return impl_->limiter_.decide_default(key, now);
}

rate_limit_decision sliding_window_rate_limiter::decide(
    const ip_address& address, std::chrono::steady_clock::time_point now) noexcept {
    char key_buffer[rate_limit_key_buffer_bytes];
    return decide(detail::rate_limit_key_for(address, key_buffer), now);
}

rate_limit_decision sliding_window_rate_limiter::decide(std::string_view key,
    const rate_limit_rule& rule, std::uintptr_t scope,
    std::chrono::steady_clock::time_point now) noexcept {
    if (impl_ == nullptr || rule.max_requests_ == 0 || rule.window_.count() <= 0) {
        return rate_limit_decision{};
    }
    return impl_->limiter_.decide_rule({.route_ = scope == 0 ? 2U : scope, .rule_ = 1U}, key, rule, now);
}

rate_limit_decision sliding_window_rate_limiter::decide(const ip_address& address,
    const rate_limit_rule& rule, std::uintptr_t scope,
    std::chrono::steady_clock::time_point now) noexcept {
    char key_buffer[rate_limit_key_buffer_bytes];
    return decide(detail::rate_limit_key_for(address, key_buffer), rule, scope, now);
}

}  // namespace ruvia

namespace ruvia::detail {
}  // namespace ruvia::detail

// servable file recorded with the metadata a response needs, and lookups after
// that touch only the index. A request never walks or stats the tree: a
// file-backed response opens its one indexed entry on the blocking pool and
// checks the handle against the indexed identity.

namespace ruvia {
namespace {

// A relative path (generic '/'-separated form) whose first component or any
// component after a '/' begins with '.' is hidden. Serving these by default
// leaks .env, .git/config, .htpasswd and similar secrets that happen to sit
// under a document root.
[[nodiscard]] bool has_hidden_path_segment(std::string_view relative_generic) noexcept {
    return relative_generic.starts_with('.') || (relative_generic.find("/.") != std::string_view::npos);
}

[[nodiscard]] detail::static_root_state* make_static_root_state(detail::static_root_config_storage config) {
    return new detail::static_root_state(std::move(config));
}

[[nodiscard]] std::filesystem::path canonical_static_root_path(const std::filesystem::path& root) {
    std::error_code ec;
    auto canonical_root = std::filesystem::weakly_canonical(root, ec);
    if (ec || !std::filesystem::is_directory(canonical_root, ec)) {
        throw std::invalid_argument("static file root not found");
    }
    return canonical_root;
}

[[nodiscard]] const detail::static_root_entry* find_static_root_entry(
    const std::vector<detail::static_root_entry>& entries,
    std::string_view relative_path) noexcept {
    return detail::find_static_index_entry(entries, relative_path,
        [](const detail::static_root_entry& entry_value) noexcept {
            return std::string_view(entry_value.relative_path_);
        });
}

[[nodiscard]] bool contains_static_directory(
    const std::vector<std::string>& directories, std::string_view relative_path) noexcept {
    return detail::find_static_index_entry(directories, relative_path,
               [](const std::string& directory) noexcept { return std::string_view(directory); }) != nullptr;
}

[[nodiscard]] detail::static_file_version static_root_entry_version(
    const detail::static_root_entry& entry_value) noexcept {
    return {entry_value.modified_seconds_, entry_value.modified_token_};
}

[[nodiscard]] bool same_static_root_entry_metadata(
    const detail::static_root_entry& left, const detail::static_root_entry& right) noexcept {
    return left.relative_path_ == right.relative_path_ && left.file_path_ == right.file_path_ &&
           left.content_type_ == right.content_type_ && left.size_ == right.size_ &&
           left.identity_ == right.identity_ && left.modified_token_ == right.modified_token_ &&
           left.modified_seconds_ == right.modified_seconds_ && left.etag_ == right.etag_ &&
           left.last_modified_ == right.last_modified_ &&
           left.directly_servable_ == right.directly_servable_;
}

[[nodiscard]] const detail::static_root_entry* find_fresh_sidecar_entry(
    const detail::static_root_state& state_value, const detail::static_root_entry& identity,
    std::string_view suffix) {
    if (!state_value.has_sidecars_) {
        return nullptr;
    }
    const detail::inline_text_concat<256> lookup_path(
        {identity.relative_path_, suffix});
    const auto* variant = find_static_root_entry(state_value.entries_, lookup_path.view());
    if (variant == nullptr ||
        !static_root_entry_version(*variant).at_least_as_new_as(static_root_entry_version(identity))) {
        return nullptr;
    }
    return variant;
}

[[nodiscard]] const detail::static_root_memory_variant* find_memory_variant(
    const detail::static_root_entry& entry_value, http_content_coding coding) noexcept {
    for (const auto& variant : entry_value.memory_variants_) {
        if (variant.content_coding_ == coding) {
            return &variant;
        }
    }
    return nullptr;
}

// Reuses an unchanged file's encoded bytes from the previous snapshot. The
// buffer is immutable, so the new snapshot shares it instead of copying it.
void share_memory_variant(detail::static_root_entry& entry_value,
    const detail::static_root_memory_variant& source_value) {
    auto& stored = entry_value.memory_variants_.emplace_back();
    stored.content_coding_ = source_value.content_coding_;
    stored.bytes_ = source_value.bytes_;
    stored.modified_token_ = source_value.modified_token_;
    stored.modified_seconds_ = source_value.modified_seconds_;
    stored.etag_ = source_value.etag_;
    stored.last_modified_ = source_value.last_modified_;
}

// The path an indexed file is opened by: on POSIX the part beneath the
// retained root handle (every entry path is the canonical root joined with
// its relative path, so it starts right after the root and its separator),
// on Windows the canonical path itself.
[[nodiscard]] const http_native_path_char_type* static_root_open_path(
    [[maybe_unused]] const detail::static_root_state& state_value, const http_native_path_char_type* file_path) noexcept {
#if defined(__unix__)
    const auto& root_path = state_value.root_;
    return file_path + root_path.size() + (root_path.ends_with('/') ? 0 : 1);
#else
    return file_path;
#endif
}

// Opens an indexed file the way every use does (static_root_entry_open is the
// per-request form). On POSIX it is opened beneath the retained root handle
// without following a symlink in any component, so a path swapped after
// indexing cannot reach outside the root. Windows opens the canonical path;
// the identity check against the index is its guard.
[[nodiscard]] detail::native_file_descriptor open_static_root_file(const detail::static_root_state& state_value,
    const http_native_path_char_type* file_path, std::error_code& ec) noexcept {
#if defined(__unix__)
    return detail::open_native_file_beneath(
        state_value.root_directory_->get(), static_root_open_path(state_value, file_path), ec);
#else
    return detail::open_native_file(static_root_open_path(state_value, file_path), ec);
#endif
}

// A file that changed (or vanished) since it was indexed yields nothing: only
// that entry loses its variants and stays file-backed, validated per request.
[[nodiscard]] std::optional<std::string> read_static_root_entry_bytes(const detail::static_root_state& state_value,
    const detail::static_root_entry& entry_value) {
    if (entry_value.size_ > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        throw std::runtime_error("static file is too large to buffer");
    }
    const auto size = static_cast<std::size_t>(entry_value.size_);
    // Read and revalidate through one handle: the kept bytes are exactly those
    // of the indexed snapshot, with no path re-resolution in between.
    std::error_code ec;
    const auto input = open_static_root_file(state_value, entry_value.file_path_.c_str(), ec);
    if (ec || detail::validate_native_file(input.get(), entry_value.identity_, entry_value.size_)) {
        return std::nullopt;
    }
    std::string bytes;
    bytes.resize(size);
    if ((size != 0 && detail::read_native_file_at(input.get(), 0, bytes.data(), size)) ||
        detail::validate_native_file(input.get(), entry_value.identity_, entry_value.size_)) {
        return std::nullopt;
    }
    return bytes;
}

void store_precompressed_variant(detail::static_root_entry& entry_value, http_content_coding coding,
    std::string encoded, bool emit_response_validators) {
    auto bytes = std::make_shared<std::string>(std::move(encoded));
    auto& variant = entry_value.memory_variants_.emplace_back();
    variant.content_coding_ = coding;
    variant.modified_token_ = entry_value.modified_token_;
    variant.modified_seconds_ = entry_value.modified_seconds_;
    variant.bytes_ = std::move(bytes);
    if (emit_response_validators) {
        variant.etag_ = detail::make_static_file_snapshot_etag(variant.bytes_->size(), entry_value.modified_token_, entry_value.identity_, coding);
        variant.last_modified_ = entry_value.last_modified_;
    }
}

template <typename t_type>
void hash_value(std::uint64_t& hash, t_type value) noexcept {
    static_assert(std::is_integral_v<t_type>);
    hash = fnv1a_accumulate_integer(hash, static_cast<std::uint64_t>(value));
}

[[nodiscard]] std::uint64_t static_root_fingerprint(const detail::static_root_state& state_value) noexcept {
    std::uint64_t hash = fnv1a_offset_basis;
#if defined(__unix__)
    // A root directory replaced by another one with an identical index is
    // still a different snapshot: requests open entries relative to it.
    hash_value(hash, state_value.root_identity_.device_);
    hash_value(hash, state_value.root_identity_.inode_);
#endif
    hash_value(hash, state_value.entries_.size());
    for (const auto& entry : state_value.entries_) {
        hash = fnv1a_accumulate(hash, entry.relative_path_);
        hash_value(hash, entry.size_);
        hash_value(hash, entry.modified_token_);
        hash_value(hash, entry.modified_seconds_);
        hash_value(hash, entry.identity_.requires_validation());
        for (const auto word : entry.identity_.words()) {
            hash_value(hash, word);
        }
        hash_value(hash, entry.directly_servable_);
    }
    return hash;
}

}  // namespace

detail::static_root_config_storage detail::static_root_access::copy_config(
    const static_root& root) {
    return static_root_config_storage(root.state_->config_);
}

std::string_view detail::static_root_access::index_file(const static_root& root) noexcept {
    return root.state_->config_.index_file_;
}

bool detail::static_root_access::has_directory_index(const static_root& root) noexcept {
    return !root.state_->config_.index_file_.empty();
}

bool detail::static_root_access::has_sidecars(const static_root& root) noexcept {
    return root.state_->has_sidecars_;
}

std::optional<detail::static_root_entry_view> detail::static_root_access::find(
    const static_root& root, std::string_view relative_path) noexcept {
    auto entry_value = find_variant(root, relative_path);
    if (!entry_value.has_value() || !entry_value->directly_servable_) {
        return std::nullopt;
    }
    return entry_value;
}

std::optional<detail::static_root_entry_view> detail::static_root_access::find_variant(
    const static_root& root, std::string_view relative_path) noexcept {
    const auto& state_value = *root.state_;
    const auto& entries = state_value.entries_;
    const auto* const entry_value = find_static_root_entry(entries, relative_path);
    if (entry_value == nullptr) {
        return std::nullopt;
    }
    return detail::static_root_entry_view(entry_value->file_path_.c_str(), entry_value->content_type_,
        state_value.config_.cache_control_, entry_value->etag_, entry_value->last_modified_, entry_value->size_, entry_value->identity_,
        entry_value->modified_token_, entry_value->modified_seconds_, state_value.config_.range_requests_,
        state_value.config_.response_validators_, entry_value->directly_servable_, &entry_value->memory_variants_,
        entry_value->resident_bytes_.get());
}

bool detail::static_root_access::is_indexed_directory(
    const static_root& root, std::string_view relative_path) noexcept {
    if (!has_directory_index(root)) {
        return false;
    }
    return contains_static_directory(root.state_->directories_, relative_path);
}

namespace {

[[nodiscard]] std::optional<detail::static_root_entry_view> resolve_normalized_static_root_entry(
    const static_root& root, bool directory_slash, std::string_view& relative, std::string& storage) {
    auto entry_value =
        directory_slash ? std::optional<detail::static_root_entry_view>{} : detail::static_root_access::find(root, relative);
    if (!entry_value.has_value() && detail::static_root_access::is_indexed_directory(root, relative)) {
        // A normalized path never ends with '/'.
        const auto index = detail::static_root_access::index_file(root);
        // reserve() may invalidate a normalized path borrowed from storage.
        const bool copy_relative = relative.data() != storage.data();
        storage.reserve(relative.size() + (relative.empty() ? 0U : 1U) + index.size());
        if (copy_relative) {
            storage.assign(relative.data(), relative.size());
        }
        if (!storage.empty()) {
            storage.push_back('/');
        }
        storage.append(index.data(), index.size());
        relative = storage;
        entry_value = detail::static_root_access::find(root, relative);
    }
    return entry_value;
}

}  // namespace

std::optional<detail::static_root_entry_view> detail::static_root_access::resolve(
    const static_root& root, std::string_view& relative, std::string& storage) {
    const bool directory_slash =
        !relative.empty() && (relative.back() == '/' || relative.back() == '\\');
    relative = normalize_static_relative_path(relative, storage);
    if (relative.empty() && !has_directory_index(root)) {
        throw_invalid_static_file_path();
    }
    return resolve_normalized_static_root_entry(root, directory_slash, relative, storage);
}

std::optional<detail::static_root_entry_view> detail::static_root_access::try_resolve(
    const static_root& root, std::string_view& relative, std::string& storage) {
    const bool directory_slash =
        !relative.empty() && (relative.back() == '/' || relative.back() == '\\');
    const auto normalized = try_normalize_static_relative_path(relative, storage);
    if (!normalized.has_value()) {
        return std::nullopt;
    }
    relative = *normalized;
    if (relative.empty() && !has_directory_index(root)) {
        return std::nullopt;
    }
    return resolve_normalized_static_root_entry(root, directory_slash, relative, storage);
}

std::uint64_t detail::static_root_access::fingerprint(const static_root& root) noexcept {
    return root.state_->fingerprint_;
}

void detail::static_root_access::install_precompressed_variants(
    static_root& root, const static_root* previous, const static_root_precompression_options& options) {
    if (!options.enabled()) {
        return;
    }
    if (options.min_bytes_ == 0 || options.max_bytes_ < options.min_bytes_) {
        throw std::invalid_argument("invalid static root precompression options");
    }

    auto& state_value = *root.state_;
    const auto* previous_state = previous == nullptr ? nullptr : previous->state_.get();
    const bool emit_response_validators =
        state_value.config_.response_validators_ == static_response_validator_policy::emit;

    for (auto& entry : state_value.entries_) {
        if (!entry.directly_servable_ || entry.size_ < options.min_bytes_ ||
            entry.size_ > options.max_bytes_ ||
            !is_http_media_type_compressible(entry.content_type_)) {
            continue;
        }

        const detail::static_root_entry* previous_entry = nullptr;
        if (previous_state != nullptr) {
            previous_entry = find_static_root_entry(previous_state->entries_, entry.relative_path_);
            if (previous_entry != nullptr && !same_static_root_entry_metadata(entry, *previous_entry)) {
                previous_entry = nullptr;
            }
        }

        std::optional<std::string> plain;
        const std::string* plain_bytes = entry.resident_bytes_.get();
        for (const auto& sidecar : detail::static_precompression_sidecars) {
            const auto coding = sidecar.content_coding_;
            if (!options.enabled(coding) || find_fresh_sidecar_entry(state_value, entry, sidecar.suffix_) != nullptr) {
                continue;
            }
            if (previous_entry != nullptr) {
                if (const auto* previous_variant = find_memory_variant(*previous_entry, coding);
                    previous_variant != nullptr) {
                    share_memory_variant(entry, *previous_variant);
                    continue;
                }
            }
            if (plain_bytes == nullptr) {
                plain = read_static_root_entry_bytes(state_value, entry);
                if (!plain.has_value()) {
                    entry.memory_variants_.clear();
                    break;
                }
                plain_bytes = &*plain;
            }
            if (plain_bytes->empty()) {
                continue;
            }
            auto encoded = encode_http_content(
                coding, *plain_bytes, {.max_encoded_bytes_ = plain_bytes->size() - 1});
            if (encoded.has_value()) {
                store_precompressed_variant(entry, coding, std::move(*encoded),
                    emit_response_validators);
            }
        }
    }
}

void detail::static_root_access::install_resident_bodies(
    static_root& root, const static_root* previous, const static_root_resident_options& options) {
    if (!options.enabled()) {
        return;
    }
    auto& state_value = *root.state_;
    const auto* previous_state = previous == nullptr ? nullptr : previous->state_.get();
    std::size_t used = 0;
    for (auto& entry : state_value.entries_) {
        if (entry.size_ > options.max_file_bytes_ || entry.size_ > options.budget_bytes_ - used) {
            continue;
        }
        const auto size = static_cast<std::size_t>(entry.size_);
        if (previous_state != nullptr) {
            const auto* previous_entry = find_static_root_entry(previous_state->entries_, entry.relative_path_);
            if (previous_entry != nullptr && previous_entry->resident_bytes_ != nullptr &&
                same_static_root_entry_metadata(entry, *previous_entry)) {
                entry.resident_bytes_ = previous_entry->resident_bytes_;
                used += size;
                continue;
            }
        }
        auto bytes = read_static_root_entry_bytes(state_value, entry);
        if (!bytes.has_value()) {
            continue;
        }
        entry.resident_bytes_ = std::make_shared<std::string>(std::move(*bytes));
        used += size;
    }
}

detail::static_root_entry_open detail::static_root_access::entry_open(
    const static_root& root, const static_root_entry_view& entry, bool checked) {
    const auto& state_value = *root.state_;
    http_native_path_string_type open_path(static_root_open_path(state_value, entry.file_path()));
#if defined(__unix__)
    return static_root_entry_open(state_value.root_directory_, std::move(open_path), entry.identity(), entry.size(), checked);
#else
    return static_root_entry_open(std::move(open_path), entry.identity(), entry.size(), checked);
#endif
}

http_response_file_descriptor detail::static_root_entry_open::operator()() const noexcept {
    std::error_code ec;
#if defined(__unix__)
    auto input = detail::open_native_file_beneath(root_directory_->get(), open_path_.c_str(), ec);
#else
    auto input = detail::open_native_file(open_path_.c_str(), ec);
#endif
    if (ec || (checked_ && detail::validate_native_file(input.get(), identity_, size_))) {
        return {};
    }
    return adopt_response_file_handle(std::move(input));
}

bool detail::static_root_access::same_snapshot(
    const static_root& left, const static_root& right) noexcept {
    const auto& lhs = *left.state_;
    const auto& rhs = *right.state_;
    if (lhs.root_ != rhs.root_ || lhs.directories_ != rhs.directories_ ||
        lhs.entries_.size() != rhs.entries_.size()) {
        return false;
    }
#if defined(__unix__)
    if (lhs.root_identity_ != rhs.root_identity_) {
        return false;
    }
#endif
    if (lhs.config_ != rhs.config_) {
        return false;
    }
    for (std::size_t i = 0; i < lhs.entries_.size(); ++i) {
        if (!same_static_root_entry_metadata(lhs.entries_[i], rhs.entries_[i])) {
            return false;
        }
    }
    return true;
}

namespace {

[[nodiscard]] std::unique_ptr<detail::static_root_state>
build_static_root_state(const std::filesystem::path& root, detail::static_root_config_storage config_storage) {
    const auto canonical_root = canonical_static_root_path(root);
    std::unique_ptr<detail::static_root_state> state(make_static_root_state(std::move(config_storage)));
    std::error_code ec;
    auto& state_value = *state;
    http_assign_native_path(state_value.root_, canonical_root);
#if defined(__unix__)
    {
        auto root_directory = detail::open_native_directory(state_value.root_.c_str(), ec);
        if (ec) {
            throw std::filesystem::filesystem_error("open static file root", canonical_root, ec);
        }
        state_value.root_identity_ = detail::native_directory_handle_identity(root_directory.get(), ec);
        if (ec) {
            throw std::filesystem::filesystem_error("inspect static file root", canonical_root, ec);
        }
        state_value.root_directory_ = std::make_shared<detail::native_file_descriptor>(std::move(root_directory));
    }
#endif

    const auto& config = state_value.config_;
    const auto serve_dotfiles = detail::static_root_serves_dotfiles(config.dotfiles_);
    if (!config.index_file_.empty()) {
        state_value.directories_.push_back({});
    }

    // Index construction is transactional. A directory may change while it is
    // being walked, but publishing a partial snapshot would turn one transient
    // filesystem error into arbitrary 404s. Let the caller keep the previous
    // root during polling, and fail startup when there is no previous snapshot.
    std::filesystem::recursive_directory_iterator iter(canonical_root, ec);
    if (ec) {
        throw std::filesystem::filesystem_error("iterate static file root", canonical_root, ec);
    }
    const std::filesystem::recursive_directory_iterator end;
    for (; iter != end; iter.increment(ec)) {
        const auto& file_path = iter->path();
        ec.clear();
        const auto status = iter->symlink_status(ec);
        if (ec) {
            throw std::filesystem::filesystem_error("inspect static file root entry", file_path, ec);
        }
        if (std::filesystem::is_symlink(status)) {
            continue;
        }
        auto relative = detail::static_file_utf8_string(
            file_path.lexically_relative(canonical_root));
        if (relative.empty() || relative.starts_with("../")) {
            continue;
        }
        // Default-deny hidden paths: skip dotfiles and do not descend into
        // dot-directories (.git, .ssh, ...) so their contents are never indexed.
        if (!serve_dotfiles && has_hidden_path_segment(relative)) {
            if (std::filesystem::is_directory(status)) {
                iter.disable_recursion_pending();
            }
            continue;
        }
        if (std::filesystem::is_directory(status)) {
            if (!config.index_file_.empty()) {
                state_value.directories_.push_back(std::move(relative));
            }
            continue;
        }
        if (!std::filesystem::is_regular_file(status)) {
            continue;
        }
        const auto extension = detail::lower_static_file_extension_from_utf8(relative);
        const bool is_sidecar_extension = detail::is_precompressed_sidecar_extension(extension);
        const bool directly_servable = detail::file_type_allowed(extension, config);
        bool usable_as_sidecar = false;
        if (!directly_servable && is_sidecar_extension) {
            const auto stem_view =
                std::string_view(relative).substr(0, relative.size() - extension.size());
            usable_as_sidecar = detail::file_type_allowed(
                detail::lower_static_file_extension_from_utf8(stem_view), config);
        }
        if (!directly_servable && !usable_as_sidecar) {
            continue;
        }
        // The entry is opened beneath the retained root, the way a request
        // opens it: a directory swapped for a symlink after it was listed
        // fails the snapshot instead of indexing a file outside the root.
        http_native_path_string_type native_path;
        http_assign_native_path(native_path, file_path);
        ec.clear();
        const auto snapshot = [&] {
            const auto input = open_static_root_file(state_value, native_path.c_str(), ec);
            return ec ? detail::response_file_snapshot{} : detail::snapshot_native_file_handle(input.get(), ec);
        }();
        if (ec) {
            throw std::filesystem::filesystem_error(
                "snapshot static file root entry", file_path, ec);
        }
        const auto emit_response_validators =
            config.response_validators_ == static_response_validator_policy::emit;
        detail::static_root_entry entry;
        entry.relative_path_ = std::move(relative);
        entry.file_path_ = std::move(native_path);
        entry.content_type_ = detail::content_type_for(extension, config);
        entry.size_ = snapshot.size_;
        entry.identity_ = snapshot.identity_;
        entry.modified_token_ = snapshot.modified_token_;
        entry.modified_seconds_ = snapshot.modified_seconds_;
        entry.directly_servable_ = directly_servable;
        if (emit_response_validators) {
            entry.etag_ = detail::make_static_file_snapshot_etag(
                snapshot.size_, snapshot.modified_token_, snapshot.identity_);
            if (const auto date = format_http_date(snapshot.modified_seconds_)) {
                entry.last_modified_.assign(date->data(), date->size());
            }
        }
        if (state_value.entries_.empty()) {
            state_value.entries_.reserve(8);
        }
        if (is_sidecar_extension) {
            state_value.has_sidecars_ = true;
        }
        state_value.entries_.push_back(std::move(entry));
    }
    if (ec) {
        throw std::filesystem::filesystem_error("iterate static file root", canonical_root, ec);
    }
    std::ranges::sort(state_value.entries_,
        [](const detail::static_root_entry& left, const detail::static_root_entry& right) {
            return left.relative_path_ < right.relative_path_;
        });
    std::ranges::sort(state_value.directories_);
    state_value.directories_.erase(
        std::ranges::unique(state_value.directories_).begin(), state_value.directories_.end());
#if defined(__unix__)
    // The walk resolved the root by path. Publish only when that path still
    // names the retained directory, so requests open entries in the indexed
    // directory rather than in one swapped in while the index was built.
    if (detail::native_directory_path_identity(state_value.root_.c_str(), ec) != state_value.root_identity_ || ec) {
        throw std::filesystem::filesystem_error("static file root changed while it was indexed", canonical_root,
            ec ? ec : std::make_error_code(std::errc::state_not_recoverable));
    }
#endif
    state_value.fingerprint_ = static_root_fingerprint(state_value);
    return state;
}

}  // namespace

std::unique_ptr<static_root> detail::static_root_access::make(
    const std::filesystem::path& root, const static_root_config_storage& config) {
    return make(root, detail::static_root_config_storage(config));
}

std::unique_ptr<static_root> detail::static_root_access::make(
    const std::filesystem::path& root, static_root_config_storage&& config) {
    auto state = build_static_root_state(root, std::move(config));
    return std::unique_ptr<static_root>(new static_root(
        std::unique_ptr<detail::static_root_state, static_root::state_deleter_type>(state.release())));
}

static_root::static_root(std::unique_ptr<detail::static_root_state, state_deleter_type> state) noexcept
    : state_(std::move(state)) {}

static_root::static_root(const std::filesystem::path& root, static_root_options options)
    : state_([&] {
          detail::validate_static_root_options(options);
          auto built = build_static_root_state(
              root, detail::store_validated_static_root_config(options));
          return std::unique_ptr<detail::static_root_state, state_deleter_type>(built.release());
      }()) {
    detail::static_root_access::install_resident_bodies(*this, nullptr,
        {.max_file_bytes_ = options.resident_max_bytes_.value_or(0),
            .budget_bytes_ = options.resident_budget_bytes_.value_or(0)});
}

static_root::~static_root() = default;

void static_root::state_deleter_type::operator()(detail::static_root_state* state_value) const noexcept {
    delete state_value;
}

std::filesystem::path static_root::path() const {
    return http_make_path_from_native_path(state_->root_);
}

namespace detail {
namespace {

void lowercase_ascii(std::string& value) noexcept {
    for (char& character : value) {
        character = static_cast<char>(http_ascii_to_lower(static_cast<unsigned char>(character)));
    }
}

inline constexpr std::string_view default_static_file_types[] = {
    "apng",
    "avif",
    "bmp",
    "css",
    "cur",
    "eot",
    "gif",
    "htm",
    "html",
    "ico",
    "jpeg",
    "jpg",
    "js",
    "json",
    "map",
    "mjs",
    "otf",
    "png",
    "svg",
    "ttf",
    "txt",
    "wasm",
    "webmanifest",
    "webp",
    "woff",
    "woff2",
    "xml",
    "xsl",
};

const static_root_mime_type_storage* find_static_mime_type(
    const std::vector<static_root_mime_type_storage>& mime_types,
    std::string_view extension) noexcept {
    return find_static_index_entry(mime_types, extension,
        [](const static_root_mime_type_storage& mime) noexcept {
            return std::string_view(mime.extension_);
        });
}

}  // namespace

static_root_config_storage store_validated_static_root_config(
    const static_root_options& source_value) {
    static_root_config_storage result;
    result.cache_control_ = source_value.cache_control_.view();
    result.index_file_ = source_value.index_file_.view();
    result.default_content_type_ = source_value.default_content_type_.view();
    result.mime_types_.reserve(source_value.mime_types_.size());
    for (const auto& mime : source_value.mime_types_) {
        auto& stored = result.mime_types_.emplace_back();
        const auto ext = mime.extension_.view();
        const bool has_dot = ext.starts_with('.');
        stored.extension_.reserve(ext.size() + (has_dot ? 0U : 1U));
        if (!has_dot) {
            stored.extension_.push_back('.');
        }
        stored.extension_.append(ext);
        lowercase_ascii(stored.extension_);
        stored.content_type_ = mime.content_type_.view();
    }
    std::ranges::sort(result.mime_types_,
        [](const static_root_mime_type_storage& left, const static_root_mime_type_storage& right) {
            return left.extension_ < right.extension_;
        });

    result.file_type_kind_ = source_value.file_types_.kind_;
    if (result.file_type_kind_ == static_file_type_policy::kind::only) {
        result.file_type_extensions_.reserve(source_value.file_types_.extensions_.size());
        for (std::string_view extension : source_value.file_types_.extensions_) {
            if (extension.starts_with('.')) {
                extension.remove_prefix(1);
            }
            auto& stored = result.file_type_extensions_.emplace_back(extension);
            lowercase_ascii(stored);
        }
        std::ranges::sort(result.file_type_extensions_);
        result.file_type_extensions_.erase(std::ranges::unique(result.file_type_extensions_).begin(),
            result.file_type_extensions_.end());
    }
    result.range_requests_ = source_value.range_requests_;
    result.response_validators_ = source_value.response_validators_;
    result.dotfiles_ = source_value.dotfiles_;
    return result;
}

bool is_valid_static_file_extension(std::string_view extension) noexcept {
    if (extension.empty() || (extension.find('/') != std::string_view::npos) ||
        (extension.find('\\') != std::string_view::npos)) {
        return false;
    }
    if (extension == "." || extension == "..") {
        return false;
    }
    if (extension.front() == '.') {
        extension.remove_prefix(1);
    }
    return !extension.empty();
}

bool file_type_allowed(std::string_view extension, const static_root_config_storage& config) {
    if (config.file_type_kind_ == static_file_type_policy::kind::all) {
        return true;
    }

    if (extension.empty() || extension == ".") {
        return false;
    }
    const auto value = extension.substr(1);
    if (config.file_type_kind_ == static_file_type_policy::kind::defaults) {
        return std::ranges::binary_search(default_static_file_types, value);
    }
    return std::ranges::binary_search(config.file_type_extensions_, value);
}

std::string content_type_for(std::string_view extension,
    const static_root_config_storage& config) {
    if (const auto* const mime = find_static_mime_type(config.mime_types_, extension); mime != nullptr) {
        return std::string(mime->content_type_);
    }

    const auto guessed = guess_static_file_content_type_from_extension(extension);
    if (guessed != std::string_view("application/octet-stream") ||
        config.default_content_type_.empty()) {
        return std::string(guessed);
    }
    return std::string(config.default_content_type_);
}

}  // namespace detail

std::optional<static_file_representation> select_static_file_representation(const static_root& root,
    std::string_view relative, const http_response_coding_selection& selection, detail::static_root_entry_view identity) {
    if (!selection.acceptable()) {
        return std::nullopt;
    }
    const bool has_sidecars = detail::static_root_access::has_sidecars(root);
    if ((!has_sidecars && !identity.has_memory_variants()) ||
        (!selection.accepts(http_content_coding::brotli) &&
            !selection.accepts(http_content_coding::zstd) &&
            !selection.accepts(http_content_coding::gzip))) {
        if (selection.identity_accepted()) {
            return static_file_representation(identity, http_content_coding::identity);
        }
        return std::nullopt;
    }

    struct candidate final {
        http_content_coding content_coding_;
        std::optional<detail::static_root_entry_view> entry_;
        std::optional<detail::static_root_memory_variant_view> memory_variant_;
    };
    std::array<candidate, detail::static_precompression_sidecars.size()> candidates{};
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        candidates[i].content_coding_ = detail::static_precompression_sidecars[i].content_coding_;
    }

    auto available = http_response_coding_candidates::identity_only();
    static constexpr std::size_t inline_path_capacity = 256;
    std::array<char, inline_path_capacity> inline_path;
    const bool use_inline_path = has_sidecars && relative.size() + 4U <= inline_path.size();
    if (use_inline_path && !relative.empty()) {
        std::memcpy(inline_path.data(), relative.data(), relative.size());
    }
    std::string variant_path;
    const detail::static_file_version identity_version{
        identity.modified_seconds(), identity.modified_token()};
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        auto& candidate = candidates[i];
        if (!selection.accepts(candidate.content_coding_)) {
            continue;
        }
        if (has_sidecars) {
            const auto suffix = detail::static_precompression_sidecars[i].suffix_;
            const std::size_t total_size = relative.size() + suffix.size();
            std::string_view lookup_path;
            if (use_inline_path) {
                std::memcpy(inline_path.data() + relative.size(), suffix.data(), suffix.size());
                lookup_path = std::string_view(inline_path.data(), total_size);
            } else {
                variant_path.reserve(total_size);
                variant_path.assign(relative.data(), relative.size());
                variant_path.append(suffix.data(), suffix.size());
                lookup_path = variant_path;
            }
            if (const auto entry_value = detail::static_root_access::find_variant(root, lookup_path);
                entry_value.has_value() &&
                detail::static_file_version{entry_value->modified_seconds(), entry_value->modified_token()}
                    .at_least_as_new_as(identity_version)) {
                candidate.entry_ = *entry_value;
                available.include(candidate.content_coding_);
                continue;
            }
        }
        if (auto memory_variant = identity.memory_variant(candidate.content_coding_);
            memory_variant.has_value()) {
            candidate.memory_variant_ = *memory_variant;
            available.include(candidate.content_coding_);
        }
    }

    if (const auto chosen = selection.select(available); chosen.acceptable()) {
        if (chosen.coding() == http_content_coding::identity) {
            return static_file_representation(identity, http_content_coding::identity);
        }
        for (const auto& candidate : candidates) {
            if (candidate.content_coding_ == chosen.coding() && candidate.entry_.has_value()) {
                return static_file_representation(*candidate.entry_, candidate.content_coding_);
            }
            if (candidate.content_coding_ == chosen.coding() &&
                candidate.memory_variant_.has_value()) {
                return static_file_representation(
                    identity, *candidate.memory_variant_, candidate.content_coding_);
            }
        }
    }

    return std::nullopt;
}

}  // namespace ruvia

namespace ruvia {
namespace {

[[nodiscard]] http_error_info normalize_http_error_info(http_error_info error) noexcept {
    auto status = error.status();
    if (!status.is_error()) {
        status = http_status::internal_server_error;
    }
    auto status_text = error.status_text();
    if (status_text.empty() || !is_valid_http_header_value(status_text)) {
        status_text = http_reason_phrase(status);
        if (status_text.empty()) {
            // Application error presentation remains a Web concern. Do not
            // invent an HTTP/1 reason phrase for an extension status code.
            status_text = "HTTP Error";
        }
    }
    auto code = error.code();
    if (code.empty()) {
        code = default_error_code(status);
    }
    auto message = error.message();
    if (message.empty()) {
        message = status_text;
    }
    return http_error_info({.status_ = status,
        .code_ = code,
        .message_ = message,
        .status_text_ = status_text,
        .validation_issues_ = error.validation_issues()});
}

[[nodiscard]] std::string serialize_error_response(http_error_info error) {
    const auto phrase = http_reason_phrase(error.status());
    const auto title = phrase.empty() ? std::string_view("HTTP Error") : phrase;
    const auto issues = error.validation_issues();
    std::size_t reserve_size = 66U + detail::json_string_size_hint(title) +
                               detail::json_string_size_hint(error.message()) +
                               detail::json_string_size_hint(error.code());
    for (const auto& issue : issues) {
        reserve_size += 30U + detail::json_string_size_hint(issue.field()) +
                        detail::json_string_size_hint(issue.code()) +
                        detail::json_string_size_hint(issue.message());
    }
    std::string out;
    out.reserve(reserve_size);
    out.append("{\"type\":\"about:blank\",\"title\":");
    detail::append_json_string(out, title);
    out.append(",\"status\":");
    http_append_unsigned(out, error.status().value());
    out.append(",\"detail\":");
    detail::append_json_string(out, error.message());
    out.append(",\"code\":");
    detail::append_json_string(out, error.code());
    if (!issues.empty()) {
        out.append(",\"errors\":[");
        for (std::size_t i = 0; i < issues.size(); ++i) {
            if (i != 0) {
                out.push_back(',');
            }
            out.append("{\"field\":");
            detail::append_json_string(out, issues[i].field());
            out.append(",\"code\":");
            detail::append_json_string(out, issues[i].code());
            out.append(",\"message\":");
            detail::append_json_string(out, issues[i].message());
            out.push_back('}');
        }
        out.push_back(']');
    }
    out.push_back('}');
    return out;
}

// `error` is already normalized.
[[nodiscard]] http_response make_normalized_error_response(http_error_info error) {
    http_response response;
    response.status(error.status());
    response.header_stable_view("Content-Type", "application/problem+json");
    response.owned_body(serialize_error_response(error));
    return response;
}

}  // namespace

void validation_issue::swap(validation_issue& other) noexcept {
    field_.swap(other.field_);
    code_.swap(other.code_);
    message_.swap(other.message_);
}

validation_issue& validation_issue::operator=(const validation_issue& other) {
    if (this != &other) {
        validation_issue candidate(other);
        swap(candidate);
    }
    return *this;
}

validation_issue& validation_issue::operator=(validation_issue&& other) noexcept {
    if (this != &other) {
        swap(other);
        other.field_.clear();
        other.code_.clear();
        other.message_.clear();
    }
    return *this;
}

http_error::http_error(http_error_info_options options)
    : status_(options.status_),
      status_text_(options.status_text_.view()),
      code_(options.code_.view()),
      message_(options.message_.view()),
      validation_issues_(options.validation_issues_.begin(), options.validation_issues_.end()) {}

http_error::http_error(http_status_code status, std::string_view code, std::string_view message,
    std::string_view status_text, std::vector<validation_issue>&& issues)
    : status_(status),
      status_text_(status_text),
      code_(code),
      message_(message),
      validation_issues_(std::move(issues)) {}

http_error& http_error::operator=(const http_error& other) {
    if (this != &other) {
        std::vector<validation_issue> copied_issues(other.validation_issues_);
        std::string copied_status_text(other.status_text_);
        std::string copied_code(other.code_);
        std::string copied_message(other.message_);
        validation_issues_.swap(copied_issues);
        status_text_.swap(copied_status_text);
        code_.swap(copied_code);
        message_.swap(copied_message);
        status_ = other.status_;
    }
    return *this;
}
const char* http_error::what() const noexcept {
    return message_.c_str();
}

http_error_info http_error::info() const& noexcept {
    return http_error_info({.status_ = status_,
        .code_ = code_,
        .message_ = message_,
        .status_text_ = status_text_,
        .validation_issues_ = validation_issues_});
}

std::string_view default_error_code(http_status_code status) noexcept {
    switch (status.value()) {
        case http_status::bad_request.value():
            return "bad_request";
        case http_status::unauthorized.value():
            return "unauthorized";
        case http_status::forbidden.value():
            return "forbidden";
        case http_status::not_found.value():
            return "not_found";
        case http_status::method_not_allowed.value():
            return "method_not_allowed";
        case http_status::conflict.value():
            return "conflict";
        case http_status::precondition_failed.value():
            return "precondition_failed";
        case http_status::content_too_large.value():
            return "content_too_large";
        case http_status::range_not_satisfiable.value():
            return "range_not_satisfiable";
        case http_status::expectation_failed.value():
            return "expectation_failed";
        case http_status::unprocessable_content.value():
            return "unprocessable_content";
        case http_status::too_many_requests.value():
            return "too_many_requests";
        case http_status::request_header_fields_too_large.value():
            return "request_header_fields_too_large";
        case http_status::internal_server_error.value():
            return "internal_error";
        case http_status::not_implemented.value():
            return "not_implemented";
        case http_status::service_unavailable.value():
            return "service_unavailable";
        case http_status::http_version_not_supported.value():
            return "http_version_not_supported";
        default:
            return status.is_server_error() ? "internal_error" : "bad_request";
    }
}

std::string exception_text(const std::exception_ptr& exception) {
    if (exception == nullptr) {
        return {};
    }
    try {
        std::rethrow_exception(exception);
    } catch (const std::system_error& error) {
        return system_error_text(error);
    } catch (const std::exception& error) {
        return std::string(error.what());
    } catch (...) {
        return std::string("non-standard exception");
    }
}

http_response detail::make_default_error_response(http_error_info error) {
    return make_normalized_error_response(normalize_http_error_info(error));
}

http_response detail::make_route_not_found_response() {
    return make_default_error_response(
        http_error_info({.status_ = http_status::not_found, .message_ = "route not found"}));
}

// Running the application's error handler, or the default response when there
// is none. A handler that throws is answered with the default response too:
// transport output stays deterministic and no exception detail reaches the
// client.
task<http_response> detail::invoke_error_handler(
    context& context_value, http_error_info error, http_error_handler_ref handler) {
    error = normalize_http_error_info(error);

    if (handler != nullptr) {
        try {
            co_return co_await handler(context_value, error);
        } catch (const http_error& nested) {
            co_return make_default_error_response(nested.info());
        } catch (...) {
            // The error handler itself threw: nothing above it observes the
            // failure, so report it server-side, keep transport output
            // deterministic, and avoid echoing exception detail to the client.
            ruvia::report_unhandled_failure("error handler", std::current_exception());
            co_return make_default_error_response(
                http_error_info({.status_ = ruvia::http_status::internal_server_error,
                    .code_ = "error_handler_failed",
                    .message_ = "error handler failed"}));
        }
    }

    co_return make_normalized_error_response(error);
}

}  // namespace ruvia

namespace ruvia::detail {
namespace {

// One synchronous representation pass: apply CORS (`cors`, when set) and the
// listener's Alt-Svc, and run the compression policy up to its offload
// boundary, which only a worker with a blocking pool can cross.
[[nodiscard]] http_buffered_compression_step prepare_representation(const http_request& request,
    const http_response_coding_policy& policy, std::string_view alternative_service,
    const http_cors_policy* cors, http_response& response, const http_server_options& options,
    const worker_ref& worker) {
    if (cors != nullptr) {
        (void)cors->apply_response_headers(request, response);
    }
    apply_alternative_service(response, alternative_service);
    if (options.compression_.has_value()) {
        if (const auto* selection = policy.selection()) {
            return apply_response_compression(*selection, request.known_method(), response, *options.compression_,
                *options.codecs_, worker.blocking_pool() != nullptr);
        }
    }
    return {http_response_compression_status::not_applicable, false};
}

}  // namespace

http_response_preparation http_response_tail::prepare(
    http_response& response, const http_server_options& options, const worker_ref& worker) const {
    const auto& request = *request_;
    const auto first_pass = prepare_representation(
        request, coding_policy_, alternative_service_, cors(options), response, options, worker);
    if (!first_pass.offload_) {
        auto policy = coding_policy_;
        buffered_response_recovery recovery;
        const auto step = recovery.advance(policy, request, response, first_pass.status_);
        if (step.action_ == buffered_response_recovery_action::ready) {
            return http_response_preparation(prepared_application_response{
                plan_buffered_http_response_write(request.known_method(), response), false});
        }
        return http_response_preparation(policy, recovery, step);
    }
    return http_response_preparation(coding_policy_);
}

task<prepared_application_response> http_response_tail::finish_prepare(const http_response_preparation& preparation,
    http_response& response, const http_server_options& options, const route_table& routes,
    const context_services& services) const {
    const auto& request = *request_;
    const auto& deferred = std::get<http_response_preparation::deferred_type>(preparation.state_);
    auto policy = deferred.policy_;
    auto recovery = deferred.recovery_;
    const auto* pending_step = deferred.pending_step_ ? &*deferred.pending_step_ : nullptr;
    http_buffered_compression_step pass{http_response_compression_status::not_applicable, true};
    for (;;) {
        buffered_response_recovery_step step;
        if (pending_step != nullptr) {
            step = *pending_step;
            pending_step = nullptr;
        } else {
            auto compression = pass.status_;
            if (pass.offload_) {
                compression = co_await compress_response_offloaded(
                    *policy.selection(), response, services.worker(), services.get_stop_token());
            }
            step = recovery.advance(policy, request, response, compression);
        }
        if (step.action_ == buffered_response_recovery_action::ready) {
            co_return prepared_application_response{
                plan_buffered_http_response_write(request.known_method(), response), recovery.recovered()};
        }
        if (step.action_ == buffered_response_recovery_action::handle_error) {
            response = co_await routes.handle_error(request, *step.error_, services);
        }
        pass = prepare_representation(
            request, policy, alternative_service_, cors(options), response, options, services.worker());
    }
}

}  // namespace ruvia::detail

namespace ruvia {
namespace {

[[nodiscard]] bool is_csrf_token_header(std::string_view name) noexcept {
    for (const std::string_view prefix : {"sec-", "proxy-", "access-control-request-", "x-forwarded-"}) {
        if (name.size() >= prefix.size() && http_ascii_equals_ignore_case(name.substr(0, prefix.size()), prefix)) {
            return false;
        }
    }
    for (const std::string_view reserved : {
             "accept", "accept-language", "content-language", "content-type", "range",
             "accept-charset", "accept-encoding", "connection", "content-length",
             "cookie", "cookie2", "date", "dnt", "expect", "host", "keep-alive",
             "origin", "permissions-policy", "referer", "set-cookie", "te",
             "trailer", "transfer-encoding", "upgrade", "via",
             "user-agent", "priority", "save-data", "device-memory", "dpr", "width",
             "viewport-width", "downlink", "ect", "rtt", "forwarded", "x-real-ip",
             "x-requested-with"}) {
        if (http_ascii_equals_ignore_case(name, reserved)) {
            return false;
        }
    }
    return true;
}

}  // namespace

const csrf_protection_config& csrf_protection::validate(const csrf_protection_config& source_value) {
    if (!is_valid_http_header_name(source_value.cookie_name_)) {
        throw std::invalid_argument("CSRF cookie name must be a valid HTTP token");
    }
    if (!is_valid_http_header_name(source_value.header_name_)) {
        throw std::invalid_argument("CSRF header name must be a valid HTTP field name");
    }
    if (!is_csrf_token_header(source_value.header_name_)) {
        throw std::invalid_argument("CSRF token requires a script-supplied custom request field");
    }
    return source_value;
}

csrf_protection::csrf_protection()
    : csrf_protection(csrf_protection_config{}) {}

csrf_protection::csrf_protection(const csrf_protection_config& config)
    : cookie_name_(validate(config).cookie_name_),
      header_name_(config.header_name_) {}

task<void> csrf_protection::invoke(context& c, next& next_value) {
    const auto method = c.req().known_method();
    const bool safe = method == http_known_method::get || method == http_known_method::head ||
                      method == http_known_method::options;
    const auto cookie = c.req().cookie(cookie_name_);
    if (!safe) {
        const auto header_value = c.req().header(header_name_);
        if (!cookie || cookie->empty() || !header_value || header_value->empty() ||
            !detail::constant_time_bytes_equal(*cookie, *header_value)) {
            throw http_error({.status_ = ruvia::http_status::forbidden,
                .code_ = "csrf_token_mismatch",
                .message_ = "CSRF token missing or invalid"});
        }
    } else if (!cookie || cookie->empty()) {
        std::array<char, detail::secure_token_size> buffer;
        const auto token = detail::generate_secure_token(buffer);
        const auto connection = c.conn();
        const cookie_options options{
            .path_ = "/",
            .same_site_ = cookie_same_site::lax,
            .secure_ = connection.scheme() == http_scheme::https ? cookie_attribute_policy::emit
                                                                 : cookie_attribute_policy::omit,
        };
        c.policy().set_cookie({.name_ = cookie_name_, .value_ = token, .attributes_ = options});
    }
    co_await next_value();
}

}  // namespace ruvia

namespace ruvia {

namespace {

constexpr std::string_view content_security_policy_name = "Content-Security-Policy";
constexpr std::string_view referrer_policy_name = "Referrer-Policy";
constexpr std::string_view permissions_policy_name = "Permissions-Policy";

[[nodiscard]] bool emits_default_security_header(default_security_header_policy policy) {
    switch (policy) {
        case default_security_header_policy::emit_default:
            return true;
        case default_security_header_policy::omit:
            return false;
        default:
            throw std::invalid_argument("default security header policy is invalid");
    }
}

[[nodiscard]] bool replaces_existing_security_header(security_header_conflict_policy policy) {
    switch (policy) {
        case security_header_conflict_policy::preserve_existing:
            return false;
        case security_header_conflict_policy::replace_existing:
            return true;
        default:
            throw std::invalid_argument("security header conflict policy is invalid");
    }
}

}  // namespace

security_headers_middleware::security_headers_middleware()
    : security_headers_middleware(security_headers_config{}) {}

security_headers_middleware::security_headers_middleware(const security_headers_config& source_value)
    : emit_content_type_options_(emits_default_security_header(source_value.content_type_options_header_)),
      emit_frame_options_(emits_default_security_header(source_value.frame_options_header_)),
      emit_strict_transport_security_(
          emits_default_security_header(source_value.strict_transport_security_header_)),
      emit_disabled_xss_protection_([&] {
          switch (source_value.xss_protection_header_) {
              case xss_protection_header_policy::emit_disabled:
                  return true;
              case xss_protection_header_policy::omit:
                  return false;
              default:
                  throw std::invalid_argument("X-XSS-Protection header policy is invalid");
          }
      }()),
      replace_existing_(replaces_existing_security_header(source_value.existing_headers_)) {
    const auto validate_value = [](std::string_view value) {
        if (!value.empty() && !is_valid_http_header_value(value)) {
            throw std::invalid_argument("security header value is invalid");
        }
    };
    validate_value(source_value.content_security_policy_);
    validate_value(source_value.referrer_policy_);
    validate_value(source_value.permissions_policy_);
    for (const auto& header : source_value.custom_headers_) {
        if (!is_valid_http_header_name(header.name_)) {
            throw std::invalid_argument("custom security header name is invalid");
        }
        if (!is_valid_http_header_value(header.value_)) {
            throw std::invalid_argument("custom security header value is invalid");
        }
    }
    content_security_policy_.assign(source_value.content_security_policy_);
    referrer_policy_.assign(source_value.referrer_policy_);
    permissions_policy_.assign(source_value.permissions_policy_);
    custom_headers_.reserve(source_value.custom_headers_.size());
    for (const auto& header : source_value.custom_headers_) {
        custom_headers_.emplace_back(header.name_, header.value_);
    }
}

task<void> security_headers_middleware::invoke(context& context_value, next& next_value) {
    const bool secure_transport = context_value.conn().scheme() == http_scheme::https;
    const auto set_header = [&context_value, replace_existing = replace_existing_](
                                std::string_view name, std::string_view value, bool skip_empty) {
        if (skip_empty && value.empty()) {
            return;
        }
        detail::context_access::security_header(context_value, name, value, replace_existing);
    };

    if (emit_content_type_options_) {
        set_header("X-Content-Type-Options", "nosniff", true);
    }
    if (emit_frame_options_) {
        set_header("X-Frame-Options", "DENY", true);
    }
    if (emit_strict_transport_security_ && secure_transport) {
        set_header("Strict-Transport-Security", "max-age=31536000; includeSubDomains", true);
    }
    if (emit_disabled_xss_protection_) {
        set_header("X-XSS-Protection", "0", true);
    }

    set_header(content_security_policy_name, content_security_policy_, true);
    set_header(referrer_policy_name, referrer_policy_, true);
    set_header(permissions_policy_name, permissions_policy_, true);

    for (const auto& header : custom_headers_) {
        if (!secure_transport &&
            http_ascii_equals_ignore_case(header.name(), "Strict-Transport-Security")) {
            continue;
        }
        set_header(header.name(), header.value(), false);
    }
    co_await next_value();
}

}  // namespace ruvia

namespace {

constexpr const char* response_stream_lane_in_progress = "response stream output operation is already in progress";

// What an output operation of a response stream borrows from its writer.
struct response_stream_output final {
    void* target_;
    bool& active_;
    ruvia::detail::operation_interrupt_source& interrupts_;
    const ruvia::worker_ref& worker_;
    ruvia::context* context_;
};

template <typename operation_type>
auto run_response_stream_output(response_stream_output output, ruvia::operation_options operation,
    const char* operation_name, operation_type start) -> decltype(start()) {
    auto lease = ruvia::detail::claim_operation_lane(output.active_, response_stream_lane_in_progress);
    ruvia::detail::operation_interrupt interrupt(
        output.interrupts_, output.worker_, operation, output.context_, &ruvia::detail::context_access::reset_interrupted_request, operation_name);
    try {
        if constexpr (std::is_void_v<typename decltype(start())::value_type>) {
            co_await start();
            interrupt.throw_if_interrupted();
        } else {
            auto result = co_await start();
            interrupt.throw_if_interrupted();
            co_return result;
        }
    } catch (...) {
        interrupt.throw_if_interrupted();
        throw;
    }
}

}  // namespace

namespace ruvia {

sse_writer::sse_writer(const sse_writer& other) noexcept
    : writer_(other.writer_),
      registration_(other.registration_, this) {}

sse_writer::sse_writer(sse_writer&& other) noexcept
    : writer_(std::exchange(other.writer_, nullptr)),
      registration_(std::move(other.registration_), this) {}

sse_writer::sse_writer(response_stream_writer& writer) noexcept
    : writer_(writer.operation_scope_.active() ? &writer : nullptr),
      registration_(writer.operation_scope_, this, &sse_writer::expire_capability) {}

response_stream_writer& sse_writer::writer() const {
    registration_.require_active();
    return *writer_;
}

void sse_writer::expire_capability(void* target) noexcept {
    static_cast<sse_writer*>(target)->writer_ = nullptr;
}

http_tunnel& context::tunnel() const {
    const auto* output = response_output().tunnel();
    if (output == nullptr) {
        throw std::logic_error("HTTP tunnel is available only in an established CONNECT route");
    }
    return output->tunnel();
}

websocket& context::websocket() const {
    const auto* output = response_output().get_websocket();
    if (output == nullptr) {
        throw std::logic_error("websocket is not available");
    }
    return output->get_websocket();
}

response_stream_writer& context::stream() {
    const auto* output = response_output().response_stream();
    if (output == nullptr) {
        throw std::logic_error("response body is not streamable");
    }
    return output->writer();
}

sse_writer context::stream_sse() {
    auto& writer = stream();
    if (!writer.committed()) {
        set_stable_response_header("Content-Type", "text/event-stream");
        set_stable_response_header("Cache-Control", "no-cache");
    }
    return sse_writer(writer);
}

namespace {

template <typename view_type>
task<std::optional<view_type>> read_body(
    detail::callback_ref<task<std::optional<std::span<const std::byte>>>(operation_options)> read, bool& read_active,
    operation_options operation) {
    auto lease = detail::claim_operation_lane(read_active, "request body read is already in progress");
    const auto chunk = co_await read(std::move(operation));
    if (!chunk) {
        co_return std::nullopt;
    }
    if constexpr (std::same_as<view_type, std::string_view>) {
        co_return as_chars(*chunk);
    } else {
        co_return *chunk;
    }
}

task<void> write_transferred_chunk(response_stream_output output, task<void> (*write)(void*, std::string_view),
    std::string chunk, operation_options operation) {
    co_await run_response_stream_output(output, std::move(operation), "response stream write",
        [&] { return write(output.target_, chunk); });
}

task<void> end_owned(response_stream_output output, task<void> (*end)(void*, std::span<const http_header_view>),
    std::vector<http_header> fields, operation_options operation) {
    const detail::inline_http_header_views views(fields);
    co_await run_response_stream_output(output, std::move(operation), "response stream end",
        [&] { return end(output.target_, views.view()); });
}

task<timer_sleep_result> sleep_stopped_by_either(void* target,
    task<timer_sleep_result> (*sleep)(void*, std::chrono::milliseconds, const stop_token&),
    std::chrono::milliseconds duration, stop_token context_stop, stop_token stop) {
    const stop_group either(context_stop, stop);
    co_return co_await sleep(target, duration, either.token());
}

}  // namespace

task<std::optional<std::span<const std::byte>>> body_reader::read(operation_options operation) & {
    return ::ruvia::make_scoped_operation(operation_scope_, read_body<std::span<const std::byte>>(read_, read_active_, std::move(operation)));
}

task<std::optional<std::string_view>> body_reader::text(operation_options operation) & {
    return ::ruvia::make_scoped_operation(operation_scope_, read_body<std::string_view>(read_, read_active_, std::move(operation)));
}

void response_stream_writer::require_output() const {
    require_active();
    if (worker_ == nullptr || context_ == nullptr) {
        throw std::logic_error("response stream is not bound to a request");
    }
}

task<stream_commit_result> response_stream_writer::commit(operation_options operation) & {
    require_output();
    return ::ruvia::make_scoped_operation(operation_scope_,
        run_response_stream_output({target_, output_active_, *interrupts_, *worker_, context_}, std::move(operation), "response stream commit",
            [target = target_, commit = commit_] { return commit(target); }));
}

task<stream_commit_result> sse_writer::commit(operation_options operation) & {
    return writer().commit(std::move(operation));
}

task<void> response_stream_writer::write(std::span<const std::byte> chunk, operation_options operation) & {
    return write(as_chars(chunk), std::move(operation));
}

task<void> response_stream_writer::write(std::string_view chunk, operation_options operation) & {
    require_active();
    return write_owned(std::string(chunk), std::move(operation));
}

task<void> response_stream_writer::write_owned(std::string&& chunk, operation_options operation) {
    require_output();
    return ::ruvia::make_scoped_operation(operation_scope_,
        write_transferred_chunk({target_, output_active_, *interrupts_, *worker_, context_}, write_, std::move(chunk), std::move(operation)));
}

task<void> response_stream_writer::writeln(std::string_view chunk, operation_options operation) & {
    require_active();
    std::string owned;
    owned.reserve(chunk.size() + 1U);
    owned.append(chunk);
    owned.push_back('\n');
    return write_owned(std::move(owned), std::move(operation));
}

task<timer_sleep_result> response_stream_writer::sleep(timeout_duration duration, stop_token stop) & {
    require_active();
    if (!stop.stoppable()) {
        return ::ruvia::make_scoped_operation(operation_scope_, sleep_(target_, duration.value(), stop_token_));
    }
    return ::ruvia::make_scoped_operation(
        operation_scope_, sleep_stopped_by_either(target_, sleep_, duration.value(), stop_token_, std::move(stop)));
}

task<void> response_stream_writer::end(std::span<const http_header_view> trailers, operation_options operation) & {
    require_output();
    std::vector<http_header> owned_trailers_value;
    owned_trailers_value.reserve(trailers.size());
    for (const auto& header : trailers) {
        owned_trailers_value.emplace_back(header.name(), header.value());
    }
    return ::ruvia::make_scoped_operation(
        operation_scope_, end_owned({target_, output_active_, *interrupts_, *worker_, context_}, end_, std::move(owned_trailers_value), std::move(operation)));
}

task<timer_sleep_result> sse_writer::sleep(timeout_duration duration, stop_token stop) & {
    return writer().sleep(duration, std::move(stop));
}

task<void> sse_writer::end(std::span<const http_header_view> trailers, operation_options operation) & {
    return writer().end(trailers, std::move(operation));
}

task<void> sse_writer::write(const sse_message& message, operation_options operation) & {
    auto& stream_writer = writer();
    auto frame = format_sse_message(message);
    return stream_writer.write(std::move(frame), std::move(operation));
}

task<std::optional<multipart_stream_part>> multipart_reader::read(operation_options operation) & {
    registration_.require_active();
    return ::ruvia::make_scoped_operation(operation_scope_, read_task(std::move(operation)));
}

body_reader& multipart_reader::get_body_reader() const {
    registration_.require_active();
    return *body_reader_;
}

void multipart_reader::expire_capability(void* target) noexcept {
    auto& reader_value = *static_cast<multipart_reader*>(target);
    reader_value.operation_scope_.close();
    reader_value.body_reader_ = nullptr;
    reader_value.state_.emplace<expired_state_type>();
}

task<std::optional<multipart_stream_part>> multipart_reader::read_task(operation_options operation) {
    auto lease = detail::claim_operation_lane(read_active_, "multipart body read is already in progress");
    if (std::holds_alternative<finished_state_type>(state_)) {
        co_return std::nullopt;
    }
    if (std::holds_alternative<failed_state_type>(state_)) {
        throw std::logic_error("multipart body consumption previously failed");
    }
    if (!std::holds_alternative<receiving_state_type>(state_)) {
        throw std::logic_error("multipart body lifetime has expired");
    }

    if (operation.stop_token_.stop_requested()) {
        detail::operation_interrupt::throw_cause(detail::operation_interrupt_cause::cancelled, "multipart body read");
    }
    const detail::operation_steps steps(std::move(operation));
    read_guard_type read_guard_value(*this);
    try {
        for (;;) {
            auto result_value = std::get<receiving_state_type>(state_).parser_.poll();
            if (const auto* part = result_value.part()) {
                read_guard_value.commit();
                co_return *part;
            }
            if (result_value.done() != nullptr) {
                while (co_await get_body_reader().read(steps.next("multipart body read"))) {
                }
                state_.emplace<finished_state_type>();
                read_guard_value.commit();
                co_return std::nullopt;
            }
            if (result_value.need_input() != nullptr) {
                auto chunk = co_await get_body_reader().read(steps.next("multipart body read"));
                if (!chunk) {
                    std::get<receiving_state_type>(state_).parser_.finish_input();
                } else {
                    std::get<receiving_state_type>(state_).parser_.feed(as_chars(*chunk));
                }
                continue;
            }
            if (const auto* failure = result_value.failure()) {
                throw failure->protocol_error();
            }
            throw std::logic_error("unexpected multipart poll result");
        }
    } catch (const std::system_error& error) {
        // An interrupted body read fed the parser nothing; parsing resumes on
        // the next read.
        if (steps.interrupted(error)) {
            read_guard_value.commit();
        }
        throw;
    }
}

}  // namespace ruvia

namespace {
ruvia::task<std::optional<ruvia::websocket_message>> read_websocket(void* target,
    ruvia::task<std::optional<ruvia::websocket_message>> (*read)(void*, ruvia::operation_options),
    bool& read_active, ruvia::operation_options operation) {
    auto lease = ruvia::detail::claim_operation_lane(read_active, "concurrent websocket reads are not supported");
    co_return co_await read(target, std::move(operation));
}

using websocket_write_type = ruvia::task<void> (*)(void*, ruvia::websocket_opcode, std::string_view, bool,
    ruvia::detail::websocket_write_extent, ruvia::operation_options);

// The final fragment releases the data lane of its fragmented message.
ruvia::task<void> write_websocket_payload(void* target, websocket_write_type write, ruvia::websocket_opcode opcode,
    std::string payload_value, bool& write_active, bool compress, ruvia::detail::websocket_write_extent extent,
    ruvia::operation_options operation, bool& fragmented_message_active) {
    auto lease = ruvia::detail::claim_operation_lane(write_active, "concurrent websocket output operations are not supported");
    co_await write(target, opcode, payload_value, compress, extent, std::move(operation));
    if (extent == ruvia::detail::websocket_write_extent::final_fragment) {
        fragmented_message_active = false;
    }
}

// The read leases are claimed by the caller before this task exists: the
// payload borrows the connection's read storage, and no read may run, even one
// started while this operation is still unstarted, until the frame has been written.
ruvia::task<void> write_websocket_message(void* target, websocket_write_type write,
    ruvia::websocket_message message, ruvia::detail::operation_lane_lease read_lease,
    ruvia::detail::operation_lane_lease message_lease, bool& write_active, bool compress,
    ruvia::operation_options operation) {
    auto held_read = std::move(read_lease);
    auto held_message = std::move(message_lease);
    auto lease = ruvia::detail::claim_operation_lane(write_active, "concurrent websocket output operations are not supported");
    co_await write(target, message.opcode(), message.payload(), compress, ruvia::detail::websocket_write_extent::message,
        std::move(operation));
}

ruvia::task<void> close_websocket_with_reason(void* target,
    ruvia::task<void> (*close)(void*, ruvia::websocket_close_options, ruvia::operation_options),
    ruvia::websocket_close_options options, std::string reason, ruvia::operation_options operation,
    bool& read_active, bool& write_active, const bool& message_send_active) {
    // The write lane also excludes another close.
    auto write_lease = ruvia::detail::claim_operation_lane(write_active, "websocket close cannot overlap an output operation");
    if (message_send_active) {
        throw std::logic_error("websocket close cannot overlap a message send");
    }
    // A pending read observes the peer Close itself; otherwise close reads,
    // and holds the read lane while it does.
    const ruvia::detail::operation_lane_lease read_lease(read_active);
    static_cast<void>(read_lease);
    options.reason_ = reason;
    co_await close(target, options, std::move(operation));
}

}  // namespace

namespace ruvia {
namespace {
detail::websocket_state& require_websocket(const std::shared_ptr<detail::websocket_state>& state) {
    if (!state) {
        throw std::logic_error("websocket owner was moved");
    }
    state->require_active();
    return *state;
}
void retire_websocket(std::shared_ptr<detail::websocket_state> state) noexcept {
    if (!state) {
        return;
    }
    if (state->owner_) {
        state->abort();
    }
    state->operation_scope_.expire();
    if (auto owner = std::exchange(state->owner_, nullptr)) {
        if (state->retire_) {
            state->retire_(owner.get());
        }
    }
}
}  // namespace
websocket::websocket(std::shared_ptr<detail::websocket_state> state) noexcept
    : state_(std::move(state)) {}
websocket::websocket(websocket&&) noexcept = default;
websocket& websocket::operator=(websocket&& other) noexcept {
    if (this != &other) {
        retire_websocket(std::exchange(state_, std::move(other.state_)));
    }
    return *this;
}
websocket::~websocket() {
    retire_websocket(std::move(state_));
}
task<std::optional<websocket_message>> websocket::read(operation_options operation) & {
    return require_websocket(state_).read(std::move(operation));
}
task<void> websocket::text(std::string_view payload, websocket_send_options options, operation_options operation) & {
    return require_websocket(state_).text(payload, options, std::move(operation));
}
task<void> websocket::binary(std::string_view payload, websocket_send_options options, operation_options operation) & {
    return require_websocket(state_).binary(payload, options, std::move(operation));
}
task<void> websocket::binary(std::span<const std::byte> payload, websocket_send_options options, operation_options operation) & {
    return require_websocket(state_).binary(payload, options, std::move(operation));
}
task<void> websocket::ping(std::string_view payload, operation_options operation) & {
    return require_websocket(state_).ping(payload, std::move(operation));
}
task<void> websocket::pong(std::string_view payload, operation_options operation) & {
    return require_websocket(state_).pong(payload, std::move(operation));
}
task<void> websocket::write_owned(websocket_opcode opcode, std::string&& payload, bool compress, operation_options operation) {
    return require_websocket(state_).write(opcode, std::move(payload), compress, std::move(operation));
}
task<void> websocket::send(websocket_message&& message, websocket_send_options options, operation_options operation) & {
    return require_websocket(state_).send(std::move(message), options, std::move(operation));
}
task<void> websocket::close(websocket_close_options options, operation_options operation) & {
    return require_websocket(state_).close(options, std::move(operation));
}
void websocket::abort() & noexcept {
    if (state_) {
        state_->abort();
    }
}
websocket_message_writer websocket::begin_text(websocket_send_options options) & {
    return require_websocket(state_).begin_text(options);
}
websocket_message_writer websocket::begin_binary(websocket_send_options options) & {
    return require_websocket(state_).begin_binary(options);
}
std::string_view websocket::subprotocol() const& noexcept {
    return state_ ? state_->subprotocol() : std::string_view{};
}
bool websocket::connected() const noexcept {
    if (!state_) {
        return false;
    }
    if (!state_->operation_scope_.active()) {
        return false;
    }
    state_->require_active();
    return state_->protocol_(state_->target_).liveness_mode() == websocket_liveness_mode::open;
}
const worker_ref& websocket::worker() const& {
    return *require_websocket(state_).worker_;
}
std::optional<std::uint16_t> websocket::peer_close_code() const noexcept {
    return state_ ? state_->peer_close_code() : std::nullopt;
}
std::string_view websocket::peer_close_reason() const& noexcept {
    return state_ ? state_->peer_close_reason() : std::string_view{};
}
websocket_termination websocket::termination() const noexcept {
    return state_ ? state_->termination() : websocket_termination::none;
}
std::uint64_t websocket::bytes_received() const noexcept {
    return state_ ? state_->bytes_received() : 0;
}
std::uint64_t websocket::bytes_sent() const noexcept {
    return state_ ? state_->bytes_sent() : 0;
}
task<std::optional<websocket_message>> detail::websocket_state::read(operation_options operation) & {
    require_active();
    operation = override_operation_options(defaults_, std::move(operation));
    if (read_timeout_) {
        operation = override_operation_options({.timeout_ = read_timeout_->value()}, std::move(operation));
    }
    return make_scoped_operation(operation_scope_, read_websocket(target_, read_, read_active_, std::move(operation)));
}

task<void> detail::websocket_state::text(std::string_view payload_value, websocket_send_options options, operation_options operation) & {
    return write(websocket_opcode::text, payload_value, options.compress_, std::move(operation));
}

task<void> detail::websocket_state::binary(std::string_view payload_value, websocket_send_options options, operation_options operation) & {
    return write(websocket_opcode::binary, payload_value, options.compress_, std::move(operation));
}

task<void> detail::websocket_state::binary(std::span<const std::byte> payload_value, websocket_send_options options, operation_options operation) & {
    return binary(as_chars(payload_value), options, std::move(operation));
}

task<void> detail::websocket_state::pong(std::string_view payload_value, operation_options operation) & {
    return write(websocket_opcode::pong, payload_value, true, std::move(operation));
}

task<void> detail::websocket_state::ping(std::string_view payload_value, operation_options operation) & {
    return write(websocket_opcode::ping, payload_value, true, std::move(operation));
}

task<void> detail::websocket_state::send(websocket_message&& message, websocket_send_options options, operation_options operation) & {
    require_active();
    operation = override_operation_options(defaults_, std::move(operation));
    if (fragmented_message_active_) {
        throw std::logic_error("a fragmented websocket message is in progress");
    }
    auto read_lease = detail::claim_operation_lane(read_active_, "websocket message send cannot overlap a read");
    auto message_lease = detail::claim_operation_lane(message_send_active_, "websocket message send is already pending");
    return make_scoped_operation(operation_scope_,
        write_websocket_message(target_, write_, message, std::move(read_lease), std::move(message_lease),
            write_active_, options.compress_, std::move(operation)));
}

task<void> detail::websocket_state::close(websocket_close_options options, operation_options operation) & {
    require_active();
    operation = override_operation_options(defaults_, std::move(operation));
    std::string owned(options.reason_.view());
    return make_scoped_operation(operation_scope_,
        close_websocket_with_reason(target_, close_, options, std::move(owned), std::move(operation),
            read_active_, write_active_, message_send_active_));
}

void detail::websocket_state::abort() & noexcept {
    if (!worker_->is_current()) {
        std::terminate();
    }
    if (!operation_scope_.active()) {
        return;
    }
    abort_(target_);
}

std::optional<std::uint16_t> detail::websocket_state::peer_close_code() const noexcept {
    if (!worker_->is_current()) {
        std::terminate();
    }
    if (!operation_scope_.active()) {
        return std::nullopt;
    }
    return peer_close_code_(target_);
}

std::string_view detail::websocket_state::peer_close_reason() const& noexcept {
    if (!worker_->is_current()) {
        std::terminate();
    }
    if (!operation_scope_.active()) {
        return {};
    }
    return peer_close_reason_(target_);
}

websocket_termination detail::websocket_state::termination() const noexcept {
    if (!worker_->is_current()) {
        std::terminate();
    }
    if (!operation_scope_.active()) {
        return websocket_termination::none;
    }
    return protocol_(target_).termination();
}

std::uint64_t detail::websocket_state::bytes_received() const noexcept {
    if (!worker_->is_current()) {
        std::terminate();
    }
    if (!operation_scope_.active()) {
        return 0;
    }
    return protocol_(target_).bytes_received();
}

std::uint64_t detail::websocket_state::bytes_sent() const noexcept {
    if (!worker_->is_current()) {
        std::terminate();
    }
    if (!operation_scope_.active()) {
        return 0;
    }
    return protocol_(target_).bytes_sent();
}

task<void> detail::websocket_state::write(websocket_opcode opcode, std::string_view payload_value, bool compress, operation_options operation,
    detail::websocket_write_extent extent) {
    require_active();
    return write(opcode, std::string(payload_value), compress, std::move(operation), extent);
}

task<void> detail::websocket_state::write(websocket_opcode opcode, std::string&& payload_value, bool compress, operation_options operation,
    detail::websocket_write_extent extent) {
    require_active();
    if (extent == detail::websocket_write_extent::message && fragmented_message_active_ &&
        (opcode == websocket_opcode::text || opcode == websocket_opcode::binary)) {
        throw std::logic_error("a fragmented websocket message is in progress");
    }
    operation = override_operation_options(defaults_, std::move(operation));
    return make_scoped_operation(operation_scope_,
        write_websocket_payload(target_, write_, opcode, std::move(payload_value), write_active_, compress, extent,
            std::move(operation), fragmented_message_active_));
}

websocket_message_writer detail::websocket_state::begin_text(websocket_send_options options) & {
    return begin_message(websocket_opcode::text, options.compress_);
}

websocket_message_writer detail::websocket_state::begin_binary(websocket_send_options options) & {
    return begin_message(websocket_opcode::binary, options.compress_);
}

websocket_message_writer detail::websocket_state::begin_message(websocket_opcode opcode, bool compress) {
    require_active();
    if (fragmented_message_active_) {
        throw std::logic_error("a fragmented websocket message is already open");
    }
    fragmented_message_active_ = true;
    ++fragmented_message_sequence_;
    return websocket_message_writer(*this, opcode, compress);
}

websocket_message_writer::websocket_message_writer(detail::websocket_state& owner, websocket_opcode opcode, bool compress) noexcept
    : owner_(&owner),
      message_id_(owner.fragmented_message_sequence_),
      opcode_(opcode),
      compress_(compress),
      registration_(owner.operation_scope_, this, &websocket_message_writer::expire_capability) {}

websocket_message_writer::websocket_message_writer(websocket_message_writer&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)),
      message_id_(other.message_id_),
      opcode_(other.opcode_),
      compress_(other.compress_),
      registration_(std::move(other.registration_), this) {}

websocket_message_writer::~websocket_message_writer() {
    abort();
}

void websocket_message_writer::expire_capability(void* target) noexcept {
    static_cast<websocket_message_writer*>(target)->owner_ = nullptr;
}

bool websocket_message_writer::message_open() const noexcept {
    return owner_ != nullptr && owner_->fragmented_message_active_ &&
           owner_->fragmented_message_sequence_ == message_id_;
}

void websocket_message_writer::abort() & noexcept {
    if (!message_open()) {
        owner_ = nullptr;
        return;
    }
    // The peer cannot tell where an unfinished message ends.
    auto* owner = std::exchange(owner_, nullptr);
    owner->fragmented_message_active_ = false;
    owner->abort();
}

task<void> websocket_message_writer::write(std::string_view chunk, operation_options operation) & {
    return submit(chunk, false, std::move(operation));
}

task<void> websocket_message_writer::write(std::span<const std::byte> chunk, operation_options operation) & {
    return submit(as_chars(chunk), false, std::move(operation));
}

task<void> websocket_message_writer::finish(std::string_view last_chunk, operation_options operation) & {
    return submit(last_chunk, true, std::move(operation));
}

task<void> websocket_message_writer::finish(std::span<const std::byte> last_chunk, operation_options operation) & {
    return submit(as_chars(last_chunk), true, std::move(operation));
}

task<void> websocket_message_writer::submit(std::string_view chunk, bool final, operation_options operation) {
    registration_.require_active();
    if (!message_open()) {
        throw std::logic_error("websocket message writer is finished");
    }
    return owner_->write(opcode_, chunk, compress_, std::move(operation),
        final ? detail::websocket_write_extent::final_fragment : detail::websocket_write_extent::fragment);
}

task<void> websocket_message_writer::submit_owned(
    std::string&& chunk, bool final, operation_options operation) {
    registration_.require_active();
    if (!message_open()) {
        throw std::logic_error("websocket message writer is finished");
    }
    return owner_->write(opcode_, std::move(chunk), compress_, std::move(operation),
        final ? detail::websocket_write_extent::final_fragment : detail::websocket_write_extent::fragment);
}

}  // namespace ruvia

namespace ruvia {

namespace {

[[nodiscard]] std::string_view byte_body_view(std::span<const std::byte> body) noexcept {
    return body.empty() ? std::string_view{}
                        : std::string_view(reinterpret_cast<const char*>(body.data()), body.size());
}

// text() and stream_text(): plain UTF-8 text plus nosniff, so a browser never
// sniffs application text into an active content type. An
// X-Content-Type-Options the application set keeps its value.
void set_default_content_type(http_response& response, http_content_type content_type) {
    if (!response.header("Content-Type")) {
        response.content_type(content_type);
    }
}

void mark_plain_text(http_response& response) {
    response.content_type(http_content_type::text_plain_utf8);
    if (!response.header("X-Content-Type-Options")) {
        response.header_stable_view("X-Content-Type-Options", "nosniff");
    }
}

void mark_plain_text_defaults(http_response& response) {
    set_default_content_type(response, http_content_type::text_plain_utf8);
    if (!response.header("X-Content-Type-Options")) {
        response.header_stable_view("X-Content-Type-Options", "nosniff");
    }
}

}  // namespace

http_response& context_response_editor::target() const {
    if (owner_->response_output().committed()) {
        throw std::logic_error("response metadata is already committed");
    }
    if (target_ == target_type::draft) {
        return owner_->response_state().draft();
    }
    if (target_ == target_type::policy) {
        return owner_->response_state().policy();
    }
    auto* response = owner_->response_state().final();
    if (response == nullptr) {
        throw std::logic_error("no selected response to edit");
    }
    return *response;
}

void context_response_editor::status(http_status_code status_code) {
    target().status(status_code);
    if (target_ == target_type::policy) {
        owner_->response_state().policy_status(status_code);
    }
}

std::string context::url_for(
    std::string_view pattern, std::span<const std::string_view> values) const {
    if (services().routes() == nullptr) {
        throw std::logic_error("url_for requires a route table bound to this context");
    }
    return services().routes()->url_for(pattern, values);
}

std::string context::url_for(
    std::string_view pattern, std::initializer_list<std::string_view> values) const {
    return url_for(pattern, std::span<const std::string_view>(values.begin(), values.size()));
}

bool context::committed() const noexcept {
    return response_output().committed();
}

void context_response_editor::remove_header(std::string_view name) {
    target().remove_header(name);
}

void context_response_editor::header(std::string_view name, std::string_view value, http_response_header_options options) {
    target().header(name, value, options);
}

void context::store_response(http_response&& response) {
    if (response_output().committed()) {
        throw std::logic_error("response is already committed");
    }
    if (&response == response_state().final()) {
        return;
    }
    response_state().finalize(std::move(response));
}

http_response context::body(std::string_view body) {
    auto response = response_state().take_draft_or_new();
    response.body(body);
    return response;
}

http_response context::body_owned(std::string&& body) {
    auto response = response_state().take_draft_or_new();
    response.owned_body(std::move(body));
    return response;
}

http_response context::body(std::span<const std::byte> body) {
    auto response = response_state().take_draft_or_new();
    response.body(byte_body_view(body));
    return response;
}

http_response context::body(static_text body) {
    auto response = response_state().take_draft_or_new();
    response.static_body(body.view());
    return response;
}

http_response context::text(std::string_view body) {
    auto response = response_state().take_draft_or_new();
    mark_plain_text_defaults(response);
    response.body(body);
    return response;
}

http_response context::text_owned(std::string&& body) {
    auto response = response_state().take_draft_or_new();
    mark_plain_text_defaults(response);
    response.owned_body(std::move(body));
    return response;
}

http_response context::text(static_text body) {
    auto response = response_state().take_draft_or_new();
    mark_plain_text_defaults(response);
    response.static_body(body.view());
    return response;
}

response_stream_writer& context::stream_text() {
    auto& writer = stream();
    if (!response_output().committed()) {
        mark_plain_text(response_state().draft());
    }
    return writer;
}

http_response context::json_serialized(std::string& body) {
    auto response = response_state().take_draft_or_new();
    set_default_content_type(response, http_content_type::application_json);
    response.owned_body(std::move(body));
    return response;
}

http_response context::html(std::string_view body) {
    auto response = response_state().take_draft_or_new();
    set_default_content_type(response, http_content_type::text_html_utf8);
    response.body(body);
    return response;
}

http_response context::html_owned(std::string&& body) {
    auto response = response_state().take_draft_or_new();
    set_default_content_type(response, http_content_type::text_html_utf8);
    response.owned_body(std::move(body));
    return response;
}

http_response context::html(static_text body) {
    auto response = response_state().take_draft_or_new();
    set_default_content_type(response, http_content_type::text_html_utf8);
    response.static_body(body.view());
    return response;
}

http_response context::error(http_error_info_options options) const {
    return error(http_error_info(options));
}

http_response context::error(const http_error_info& info) const {
    auto response = detail::make_default_error_response(info);
    return response;
}

task<http_response> context::not_found() {
    return ::ruvia::make_scoped_operation(operation_scope_, not_found_task());
}

task<http_response> context::not_found_task() {
    // Selected only here, on demand: every matched request carries the table,
    // and few of them ever ask for a not-found answer.
    const auto* routes = services().routes();
    const auto handler = routes == nullptr ? detail::http_not_found_handler_ref{nullptr}
                                           : routes->not_found_handler_for(request_.path());
    if (handler != nullptr) {
        co_return co_await handler(*this);
    }

    co_return detail::make_route_not_found_response();
}

http_response context::streaming_head() {
    auto response = response_state().take_draft_or_new();
    response_state().apply_policy(response);
    return response;
}

task<http_response> detail::context_access::commit_streaming_head(context& context_value) {
    if (context_access::has_response(context_value)) {
        throw std::logic_error("a complete response is already selected");
    }
    if (session_access::available(context_value)) {
        co_await session_access::commit(context_value);
    }
    auto response = context_value.streaming_head();
    detail::apply_alternative_service(response, context_value.services().alternative_service());
    co_return response;
}

context& context::set_stable_response_header(std::string_view name, std::string_view value) {
    if (response_output().committed()) {
        throw std::logic_error("response headers are already committed");
    }
    response_state().draft().header_stable_view(name, value);
    return *this;
}

void context::apply_response_state(
    http_response& response, std::optional<http_status_code> status_code) {
    if (!response_state().has_draft()) {
        if (status_code.has_value()) {
            response.status(*status_code);
        }
        return;
    }
    const auto& metadata = response_state().draft();
    response.status(status_code.value_or(metadata.status()));
    if (!metadata.headers().empty()) {
        response.transfer_headers_from(metadata, http_response_header_transfer::apply);
    }
}

namespace {

void write_cookie(http_response& response, std::string_view name, std::string_view value,
    const cookie_options& options) {
    const set_cookie_plan plan(name, value, options);
    response.set_cookie(plan);
}

[[nodiscard]] bool redirect_location_contains_line_break(std::string_view location) noexcept {
    for (const char ch : location) {
        if (ch == '\r' || ch == '\n') {
            return true;
        }
    }
    return false;
}

// RFC 3986 section 3.2 authority of an absolute or network-path reference:
// it follows "//" and ends at the first "/", "?" or "#". Browsers treat "\" as
// a path separator for http(s) (WHATWG URL), so a percent-encoded "\" inside
// the authority would move the host the browser contacts, e.g.
// "http://good.example\@evil.example/".
[[nodiscard]] bool redirect_authority_contains_backslash(std::string_view location) noexcept {
    std::size_t start = 0;
    if (!location.starts_with("//")) {
        const auto colon = location.find(':');
        if (colon == std::string_view::npos || colon == 0 || location.substr(colon + 1, 2) != "//") {
            return false;
        }
        for (std::size_t index = 0; index < colon; ++index) {
            const char ch = location[index];
            const bool alpha = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
            if (!alpha && (index == 0 || !((ch >= '0' && ch <= '9') || ch == '+' || ch == '-' || ch == '.'))) {
                return false;
            }
        }
        start = colon + 1;
    }
    const auto authority = location.substr(start + 2);
    return authority.substr(0, authority.find_first_of("/?#")).find('\\') != std::string_view::npos;
}

}  // namespace

void context_response_editor::set_cookie(set_cookie_options options) {
    write_cookie(target(), options.name_.view(), options.value_.view(), options.attributes_);
}

void context_response_editor::set_signed_cookie(set_signed_cookie_options options) {
    const auto name = options.name_.view();
    const auto value = options.value_.view();
    const auto secret = options.secret_.view();
    const auto prefix = options.attributes_.prefix_
                            ? std::string_view(ruvia::http_cookie_prefix_text(*options.attributes_.prefix_))
                            : std::string_view{};
    const detail::inline_text_concat<128> wire_name({prefix, name});
    char signature[detail::cookie_signature_size];
    detail::write_cookie_signature(signature, secret, wire_name.view(), value);
    const detail::inline_text_concat<256> signed_value(
        {value, ".", std::string_view(signature, sizeof(signature))});
    write_cookie(target(), name, signed_value.view(), options.attributes_);
}

void context_response_editor::delete_cookie(delete_cookie_options options) {
    options.attributes_.max_age_.reset();
    options.attributes_.expires_ = cookie_deletion_expires;
    write_cookie(target(), options.name_.view(), "", options.attributes_);
}

http_response context::redirect(redirect_response_options options) {
    const auto location = options.location_.view();
    const auto status_code = options.status_;
    if (!is_http_redirect_status(status_code)) {
        throw std::invalid_argument("redirect status must be 301, 302, 303, 307, or 308");
    }
    if (redirect_location_contains_line_break(location)) {
        throw std::invalid_argument("redirect location must not contain CR or LF");
    }
    if (redirect_authority_contains_backslash(location)) {
        throw std::invalid_argument("redirect location authority must not contain a backslash");
    }
    http_response response;
    apply_response_state(response, status_code);
    std::string encoded_location;
    response.header("Location", percent_encode_uri_reference(location, encoded_location));
    response_state().reset_draft();
    return response;
}

http_response make_health_response(context& context_value) {
    return detail::context_access::make_static_json_response(context_value, R"({"status":"ok"})");
}

http_response make_readiness_response(context& context_value, readiness_response_options options) {
    switch (options.state_) {
        case readiness_state::ready:
            return detail::context_access::make_static_json_response(
                context_value, R"({"status":"ready"})");
        case readiness_state::unavailable:
            break;
        default:
            throw std::invalid_argument("readiness state is invalid");
    }

    context_value.draft().status(http_status::service_unavailable);
    const auto reason = options.unavailable_reason_.view();
    if (reason.empty()) {
        return detail::context_access::make_static_json_response(
            context_value, R"({"status":"not_ready"})");
    }

    static constexpr std::string_view prefix = R"({"status":"not_ready","reason":)";
    std::string body;
    body.reserve(prefix.size() + detail::json_string_size_hint(reason) + 1U);
    body.append(prefix);
    detail::append_json_string(body, reason);
    body.push_back('}');
    return detail::context_access::make_owned_json_response(context_value, body);
}
}  // namespace ruvia

namespace ruvia {
void context::advertise_origins(std::span<const std::string_view> origins) {
    if (services().connection_advertisements() == nullptr || conn().tls() == nullptr) {
        throw std::logic_error("ORIGIN requires an HTTPS connection with advertisement output");
    }
    services().connection_advertisements()->advertise_origins(origins);
}
void context::advertise_alternative_service(std::string_view value) {
    if (services().connection_advertisements() == nullptr) {
        throw std::logic_error("this dispatch has no connection advertisement output");
    }
    services().connection_advertisements()->advertise_alternative_service(value);
}
bool context::push(http_push_request_view request) {
    if (services().push_output() == nullptr) {
        throw std::logic_error("server push requires an HTTP/2 request");
    }
    return services().push_output()->push(request);
}
task<void> context::inform(const http_interim_response_head& response, operation_options operation) & {
    if (services().interim_output() == nullptr) {
        throw std::logic_error("this dispatch has no transport response output");
    }
    return services().interim_output()->inform(response, std::move(operation), *this);
}
}  // namespace ruvia

namespace ruvia::detail {
void http_connection_advertisement_output::advertise_origins(std::span<const std::string_view> origins) {
    if (origins_ == nullptr) {
        throw std::logic_error("this HTTP version does not support ORIGIN frames");
    }
    if (origins.size() > max_http_header_bytes / 2) {
        throw std::length_error("too many advertised origins");
    }
    auto bytes_value = origins.size() * 2;
    for (const auto origin : origins) {
        if (origin.size() > max_http_header_bytes - std::min(bytes_value, max_http_header_bytes)) {
            throw std::length_error("origin advertisement exceeds its byte bound");
        }
        bytes_value += origin.size();
    }
    origins_(target_, origins);
}
void http_connection_advertisement_output::advertise_alternative_service(std::string_view value) {
    if (service_ == nullptr) {
        throw std::logic_error("ALTSVC frames require HTTP/2");
    }
    if (value.size() > max_http_header_bytes) {
        throw std::length_error("alternative service advertisement exceeds its byte bound");
    }
    service_(target_, value);
}
task<void> http_interim_response_output::inform(
    const http_interim_response_head& response, operation_options operation, context& context_value) {
    if (final_committed_) {
        throw std::logic_error("the final response has already started");
    }
    std::vector<http_header> owned;
    owned.reserve(response.headers().size());
    for (const auto& field : response.headers()) {
        owned.emplace_back(field.name(), field.value());
    }
    return ::ruvia::make_scoped_operation(
        scope_, write_owned(response.status(), std::move(owned), std::move(operation), context_value));
}
task<void> http_interim_response_output::write_owned(http_status_code status,
    std::vector<http_header> fields_value, operation_options operation, context& context_value) {
    auto lease = claim_operation_lane(output_active_, "interim response output is already active");
    if (final_committed_) {
        throw std::logic_error("the final response has already started");
    }
    const detail::inline_http_header_views<16> active_views(fields_value);
    const http_interim_response_head head(status, active_views.view());
    operation_interrupt interrupt(interrupts_, context_value.worker(), operation, &context_value,
        &context_access::reset_interrupted_request, "interim response write");
    try {
        co_await write_(target_, head);
    } catch (...) {
        interrupt.throw_if_interrupted();
        throw;
    }
    interrupt.throw_if_interrupted();
}
task<void> http_interim_response_output::send_continue() {
    if (final_committed_) {
        co_return;
    }
    auto lease = claim_operation_lane(output_active_, "interim response output is already active");
    co_await write_(target_, http_interim_response_head(http_status::continue_value));
}
}  // namespace ruvia::detail

namespace ruvia {

namespace {

// Subrequest framing headers are owned by dispatch, as on the wire.
constexpr std::size_t max_subrequest_header_bytes = 64 * 1024;
}  // namespace

std::optional<std::string_view> dispatch_response::header(std::string_view name) const& noexcept {
    for (const auto& header : headers_) {
        if (http_ascii_equals_ignore_case(header.name(), name)) {
            return header.value();
        }
    }
    return std::nullopt;
}

void dispatch_response::copy_head_from(const http_response& response) {
    status_ = response.status();
    if (!response.headers().empty()) {
        headers_.reserve(response.headers().size());
        for (const auto& header : response.headers()) {
            headers_.emplace_back(header.name(), header.value());
        }
    }
}

task<dispatch_response> context::dispatch(dispatch_options options) {
    if (!services().routes() || !capabilities_.worker().is_current()) {
        throw std::logic_error("dispatch requires a routed context on its owning worker");
    }
    if (services().dispatch_depth() >= 8) {
        throw std::logic_error("subrequest nesting limit exceeded");
    }
    if (!is_valid_http_method_token(options.method_) ||
        classify_http_method(options.method_) == http_known_method::connect ||
        !is_valid_http_origin_form_target(options.target_)) {
        throw std::invalid_argument("dispatch requires a valid method and origin-form target");
    }
    if (options.body_.size() > services().max_decoded_body_bytes()) {
        throw std::invalid_argument("subrequest body exceeds the configured limit");
    }
    if (options.method_.size() > max_subrequest_header_bytes ||
        options.target_.size() > max_subrequest_header_bytes - options.method_.size()) {
        throw std::invalid_argument("subrequest headers exceed the configured limit");
    }
    std::size_t header_bytes = options.method_.size() + options.target_.size();
    for (const auto& header : options.headers_) {
        if (!is_valid_http_header_name(header.name()) || !is_valid_http_header_value(header.value())) {
            throw std::invalid_argument("invalid subrequest header");
        }
        if (is_http_protocol_managed_request_field(header.name())) {
            throw std::invalid_argument("subrequest framing headers are owned by dispatch");
        }
        if (header.name().size() > max_subrequest_header_bytes - header_bytes ||
            header.value().size() > max_subrequest_header_bytes - header_bytes - header.name().size()) {
            throw std::invalid_argument("subrequest headers exceed the configured limit");
        }
        header_bytes += header.name().size() + header.value().size();
    }
    // The operation is lazy and the options borrow caller storage: take owned
    // copies now. The subrequest is built directly as a parsed request; it
    // never round-trips through a wire format.
    std::string method(options.method_.view());
    std::string target(options.target_.view());
    std::string body(options.body_.view());
    std::vector<http_header> headers;
    if (!options.headers_.empty()) {
        headers.reserve(options.headers_.size());
        for (const auto& header : options.headers_) {
            headers.emplace_back(header.name(), header.value());
        }
    }
    return ::ruvia::make_scoped_operation(operation_scope_,
        dispatch_task(std::move(method), std::move(target), std::move(body), std::move(headers),
            std::move(options.stop_token_)));
}

task<dispatch_response> context::dispatch_task(std::string method, std::string target,
    std::string body_storage, std::vector<http_header> headers,
    ::ruvia::stop_token stop_token_value) {
    const std::string_view body = body_storage;
    std::array<char, http_max_unsigned_decimal_size> content_length_text;
    const auto content_length = http_format_unsigned(content_length_text, body.size());
    detail::inline_http_header_views<16> active_headers(2U + headers.size());
    active_headers.push_back(http_header_view("Host", "localhost"));
    for (const auto& header : headers) {
        active_headers.push_back(http_header_view(header.name(), header.value()));
    }
    active_headers.push_back(http_header_view("Content-Length", content_length));
    const auto request = [&]() {
        try {
            return make_parsed_http_request(method, target, active_headers.view(),
                std::as_bytes(std::span(body.data(), body.size())));
        } catch (const http_protocol_error&) {
            throw std::invalid_argument("invalid subrequest");
        }
    }();
    const auto& routes = *services().routes();
    const auto resolution = routes.resolve(request);
    if (const auto* resolved = resolution.resolved()) {
        const auto& endpoint = resolved->route().endpoint();
        if (endpoint.buffered() == nullptr ||
            endpoint.request_body_mode() != detail::request_body_mode::buffered) {
            throw std::logic_error("dispatch requires a buffered route");
        }
    }
    const stop_group stop_owner(capabilities_.stop_token(), std::move(stop_token_value));
    const auto stop = stop_owner.token();
    if (stop.stop_requested()) {
        throw std::system_error(asio::error::operation_aborted);
    }
    // The parent request already passed the application rate limit and the
    // target-origin check of its connection.
    const detail::http_request_admission_policy policy{
        .max_buffered_body_bytes_ = services().max_decoded_body_bytes(),
        .apply_rate_limit_ = false,
        .verify_target_origin_ = false,
    };
    http_response response;
    auto admission = co_await detail::admit_and_dispatch_buffered_request(routes,
        {
            .request_ = request,
            .resolution_ = resolution,
            .coding_ = http_response_coding_selection_for(request),
        },
        policy, services().for_subrequest(stop), response);
    if (detail::http_response_needs_not_acceptable(admission.tail().coding_policy(), request, response) &&
        !response.status().is_error()) {
        response = co_await routes.handle_error(request,
            http_error_info(detail::http_not_acceptable_coding_error()), admission.services());
    }
    // The error handler answers the 406 with any response shape, so the file
    // check covers whichever response is final.
    if (response.file_body() || response.has_multipart_file_body()) {
        throw std::logic_error("dispatch requires a buffered response");
    }
    const auto known_method = request.known_method();
    {
        const auto discarded_method = std::move(method);
        const auto discarded_target = std::move(target);
        const auto discarded_body = std::move(body_storage);
        const auto discarded_headers = std::move(headers);
    }
    dispatch_response result;
    result.copy_head_from(response);
    // As on the wire: HEAD and content-forbidden statuses carry no body.
    if (!plan_http_response_body(known_method, response.status()).body_suppressed()) {
        result.body_.assign(response.body_bytes());
    }
    co_return result;
}

}  // namespace ruvia

#ifdef RUVIA_ENABLE_DATABASE
#endif

#ifdef RUVIA_ENABLE_REDIS
#endif
namespace ruvia {

context::context(const http_request& request, const detail::context_services& services)
    : context(request, {}, nullptr, nullptr, 0, 0, services) {}

context::context(const http_request& request,
    std::string_view route_path, const std::string_view* param_names,
    const std::string_view* param_values, std::size_t param_count, std::uintptr_t route_rate_limit_scope,
    const detail::context_services& services)
    : request_(request),
      capabilities_(services.worker(), services.get_stop_token(), services.worker_states()),
      route_path_(route_path),
      param_names_(param_names),
      param_values_(param_values),
      param_count_(param_count < detail::max_route_params ? param_count : detail::max_route_params),
      route_rate_limit_scope_(route_rate_limit_scope),
      request_storage_(std::make_unique<detail::context_request_storage>(services)) {
    operation_scope_.bind_worker(services.worker());
}
context::~context() = default;

const conn_info& context::conn() const& noexcept {
    return services().get_conn_info();
}

std::shared_ptr<void> context::connection_state() const noexcept {
    return services().connection_state();
}

void context::abort_connection() noexcept {
    services().abort_connection();
}

bool context::is_subrequest() const noexcept {
    return services().dispatch_depth() != 0;
}

bool context::early_data() const noexcept {
    return services().early_data();
}

detail::context_services& context::services() noexcept {
    return request_storage().services_;
}

const detail::context_services& context::services() const noexcept {
    return request_storage().services_;
}

const ruvia::env& context::env() const noexcept {
    static const ruvia::env empty;
    return services().env() != nullptr ? *services().env() : empty;
}

std::string& context::decoded_body() const {
    auto& storage = request_storage();
    if (!storage.decoded_body_) {
        storage.decoded_body_.emplace();
    }
    return *storage.decoded_body_;
}

detail::context_request_storage& context::request_storage() const {
    return *request_storage_;
}

const detail::context_request_body_source& context::request_body_source() const noexcept {
    return services().request_body_source();
}

detail::context_response_output& context::response_output() noexcept {
    return services().response_output();
}

const detail::context_response_output& context::response_output() const noexcept {
    return services().response_output();
}

detail::context_response_state& context::response_state() noexcept {
    return request_storage().response_state_;
}

const detail::context_response_state& context::response_state() const noexcept {
    return request_storage().response_state_;
}

detail::context_session_state& context::session_state() noexcept {
    return request_storage().session_state_;
}

const detail::context_session_state& context::session_state() const noexcept {
    return request_storage().session_state_;
}

detail::request_bindings& context::request_bindings() noexcept {
    return request_storage().request_bindings_;
}

const detail::request_bindings& context::request_bindings() const noexcept {
    return request_storage().request_bindings_;
}

const http_response* context::response() const noexcept {
    const auto* final = response_state().final();
    return final;
}

bool context::has_response() const noexcept {
    return response_state().final() != nullptr;
}

void context::respond(http_response&& response) {
    store_response(std::move(response));
}

#ifdef RUVIA_ENABLE_DATABASE
db_session context::db() const {
    const auto& services_value = services();
    return services_value.client_registries().db(
        operation_scope_, services_value.io_metrics(), capabilities_.stop_token());
}

db_session context::db(std::string_view alias) const {
    const auto& services_value = services();
    return services_value.client_registries().db(
        alias, operation_scope_, services_value.io_metrics(), capabilities_.stop_token());
}
#endif

#ifdef RUVIA_ENABLE_REDIS
redis_session context::redis() const {
    const auto& services_value = services();
    return services_value.client_registries().redis(
        operation_scope_, services_value.io_metrics(), capabilities_.stop_token());
}

redis_session context::redis(std::string_view alias) const {
    const auto& services_value = services();
    return services_value.client_registries().redis(
        alias, operation_scope_, services_value.io_metrics(), capabilities_.stop_token());
}
#endif

bool detail::context_response_output::committed() const noexcept {
    if (buffered() != nullptr) {
        return false;
    }
    if (const auto* stream = response_stream()) {
        return stream->writer().committed();
    }
    return true;
}

conn_info detail::context_services::resolve_conn_info(const http_request& request) const noexcept {
    auto resolved = connection_services_.info_;
    if (!connection_services_.peer_trusted_ || worker_services_->trusted_proxies_ == nullptr) {
        return resolved;
    }
    const auto forwarded =
        worker_services_->trusted_proxies_->resolve(resolved.remote().address(), request);
    if (forwarded.from_forwarded_header()) {
        resolved.apply_forwarded(forwarded.address(), forwarded.scheme());
    }
    return resolved;
}

}  // namespace ruvia

namespace ruvia {
namespace {

// Copies the file a borrowed path names into `output` in NUL-terminated
// native form: a borrowed std::filesystem::path's native string as is, text
// terminated and, on Windows, widened. False when the path cannot name a file
// (empty, an embedded NUL, or text that is not UTF-8 on Windows).
[[nodiscard]] bool assign_native_file_path(borrowed_file_path path, http_native_path_string_type& output) {
    if (const auto native = path.native(); !native.empty()) {
        output.assign(native);
        return true;
    }
    const auto text = path.text();
    if (text.empty() || text.find('\0') != std::string_view::npos) {
        return false;
    }
#if defined(_WIN32)
    if (text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return false;
    }
    const auto text_length = static_cast<int>(text.size());
    const auto wide_length =
        ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), text_length, nullptr, 0);
    if (wide_length <= 0) {
        return false;
    }
    output.resize(static_cast<std::size_t>(wide_length));
    if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), text_length, output.data(),
            wide_length) != wide_length) {
        return false;
    }
#else
    output.assign(text.data(), text.size());
#endif
    return true;
}

inline constexpr std::size_t file_response_header_reserve = 7;

[[nodiscard]] http_error static_file_changed_error() {
    return http_error({.status_ = ruvia::http_status::internal_server_error,
        .code_ = "static_file_changed",
        .message_ = "static file changed since its index was built"});
}

// context::file's open, run on the blocking pool. The path travels with it in
// the synchronized process resource -- an abandoned open frees it on the pool
// thread -- and comes back with the opened file, so the response is framed
// from the same copy.
struct opened_file_path final {
    http_native_path_string_type path_;
    detail::opened_response_file file_{};
    std::error_code error_{};
};

// Runs a file response's open (and, for an indexed file, its identity check)
// on the worker's blocking pool, never on the worker. The wait is bounded like
// the body reads the transport waits on later: the server write_timeout_ and
// the request's stop token. An abandoned open keeps running on the pool and
// frees what it owns there.
template <typename open_type>
[[nodiscard]] task<blocking_result<std::invoke_result_t<open_type&>>> run_file_open(
    context& context_value, open_type open) {
    return run_blocking(context_value.worker(),
        {.timeout_ = detail::context_access::services(context_value).file_io_timeout(),
            .stop_token_ = context_value.stop_token()},
        std::move(open));
}

// The open's value. An open that never completed is answered as a file body
// read that did not complete: 503 file_read_unavailable, after a real fault
// (no blocking pool, a stalled open) went to the connection failure outlet.
// The callable's own exception is rethrown unchanged.
template <typename value_type>
[[nodiscard]] value_type take_file_open(context& context_value, blocking_result<value_type>&& result) {
    if (result.status() != blocking_status::completed) {
        const auto failure = detail::response_file_failure::read_not_completed(result.status());
        detail::context_access::services(context_value)
            .connection_failure()
            .invoke(detail::context_access::services(context_value).remote_address(), failure.fault());
        std::rethrow_exception(failure.response_error());
    }
    return std::move(result).take_value();
}

// Names the response file by a borrowed native path that outlives
// make_file_response: a static_root index entry or context::file's opened
// copy. http_response copies the path into its own storage, so neither
// borrow reaches the returned body. The handle opened for the response goes
// with the body, so the transport reads it on the blocking pool and never
// reopens the path.
class file_response_path final {
public:
    // `path` must be NUL-terminated (a c_str() or path::native() view).
    file_response_path(http_native_path_view_type path, http_response_file_identity identity,
        http_response_file_descriptor opened)
        : path_(path),
          identity_(identity),
          opened_(std::move(opened)) {
        if (path_.empty() || !opened_) {
            throw std::logic_error("file response has no native path or opened handle");
        }
    }

    void set_body(http_response& response, std::uint64_t size, std::uint64_t offset, std::uint64_t length) {
        response.file_body(path_, size, offset, length, identity_, std::move(opened_));
    }

    void set_multipart_body(http_response& response, std::uint64_t size,
        http_multipart_byte_range_plan&& plan) {
        response.multipart_file_body(path_, size, identity_, std::move(plan), std::move(opened_));
    }

private:
    http_native_path_view_type path_;
    http_response_file_identity identity_;
    http_response_file_descriptor opened_;
};

class file_response_body_source final {
public:
    [[nodiscard]] static file_response_body_source file(file_response_path path) {
        return file_response_body_source(std::move(path));
    }

    [[nodiscard]] static file_response_body_source bytes_value(std::string_view bytes_value) {
        return file_response_body_source(bytes_value);
    }

    void set_body(http_response& response, std::uint64_t size,
        std::uint64_t offset, std::uint64_t length) {
        if (auto* path = std::get_if<file_response_path>(&value_)) {
            path->set_body(response, size, offset, length);
            return;
        }
        const auto bytes_value = std::get<std::string_view>(value_);
        if (offset > bytes_value.size() || length > bytes_value.size() - offset) {
            throw std::logic_error("static memory response slice is out of range");
        }
        // Copied, not borrowed: the snapshot is not guaranteed to outlive the
        // response. A document-root lease ends when dispatch returns, before
        // the transport writes the body, and a caller's static_root carries no
        // lease at all. Borrowing would need a per-response reference on the
        // snapshot (shared ownership on the hot path) or a lease release hook
        // inside http_response.
        std::string owned(
            bytes_value.substr(static_cast<std::size_t>(offset), static_cast<std::size_t>(length)));
        response.file_bytes_body(std::move(owned));
    }

    void set_full_body(
        http_response& response, std::uint64_t size) {
        set_body(response, size, 0, size);
    }

    void set_multipart_body(http_response& response,
        std::uint64_t size, http_multipart_byte_range_plan&& plan) {
        if (auto* path = std::get_if<file_response_path>(&value_)) {
            path->set_multipart_body(response, size, std::move(plan));
            return;
        }
        const auto source_value = std::get<std::string_view>(value_);
        std::size_t total_size = 0;
        for (const auto& segment : plan.segments()) {
            total_size += segment.kind_ == http_multipart_byte_range_plan::segment_kind::file
                              ? static_cast<std::size_t>(segment.file_length_)
                              : segment.metadata_length_;
        }
        std::string body;
        body.reserve(total_size);
        for (const auto& segment : plan.segments()) {
            if (segment.kind_ == http_multipart_byte_range_plan::segment_kind::file) {
                body.append(source_value.substr(static_cast<std::size_t>(segment.file_offset_),
                    static_cast<std::size_t>(segment.file_length_)));
            } else {
                body.append(plan.metadata().substr(segment.metadata_offset_, segment.metadata_length_));
            }
        }
        response.file_bytes_body(std::move(body));
    }

private:
    explicit file_response_body_source(file_response_path path) noexcept
        : value_(std::move(path)) {}

    explicit file_response_body_source(std::string_view bytes_value) noexcept
        : value_(bytes_value) {}

    std::variant<file_response_path, std::string_view> value_;
};

// What one file response describes: which bytes, when they last changed, and
// the policy the serving route attached to them. Fifteen positional arguments
// at a call site said none of that; designated initializers do. It is known
// before any file is opened, so the response plan is too.
struct file_response_source final {
    std::uint64_t size_{0};
    std::uint64_t modified_token_{0};
    std::time_t modified_seconds_{0};
    // What a computed ETag digests; unchecked for bytes served from memory.
    http_response_file_identity identity_{http_response_file_identity::unchecked()};
    // Never empty: the caller's type, the index entry's, or the one guessed
    // from the path.
    std::string_view content_type_;
    std::string_view cache_control_;
    static_range_request_policy range_requests_{static_range_request_policy::ignore};
    static_response_validator_policy response_validators_{static_response_validator_policy::omit};
    std::string_view precomputed_etag_;
    std::string_view precomputed_last_modified_;
    http_content_coding content_coding_{http_content_coding::identity};
    bool negotiates_encoding_{false};
};

// The validators one file response sends and evaluates preconditions
// against. The views borrow the caller's storage or the source's precomputed
// values.
struct file_validators final {
    std::string_view etag_;
    // Empty when the date is unavailable.
    std::string_view last_modified_;
    std::time_t modified_seconds_{0};
};

[[nodiscard]] file_validators compute_file_validators(const file_response_source& source_value,
    std::array<char, detail::static_file_etag_size>& etag_storage,
    std::array<char, http_imf_fixdate_size>& last_modified_storage) noexcept {
    file_validators validators;
    // RFC 9110 §8.8.2.1 forbids an origin server from emitting a
    // Last-Modified value later than the message origination time. Filesystems
    // can legitimately contain future mtimes (clock skew, archives, or an
    // explicit timestamp), so use this response's current second for the wire
    // validator and every date precondition evaluated against it. The clamped
    // value is not the representation's actual validator and therefore cannot
    // be a strong If-Range validator (RFC 9110 §13.1.5).
    const auto response_seconds = std::time(nullptr);
    const bool has_response_time = response_seconds != std::time_t{-1};
    const bool last_modified_is_actual = has_response_time && source_value.modified_seconds_ <= response_seconds;
    validators.modified_seconds_ = last_modified_is_actual ? source_value.modified_seconds_ : response_seconds;
    if (source_value.response_validators_ == static_response_validator_policy::emit) {
        if (source_value.precomputed_etag_.empty()) {
            etag_storage = detail::format_static_file_snapshot_etag(
                source_value.size_, source_value.modified_token_, source_value.identity_);
            validators.etag_ = std::string_view(etag_storage.data(), etag_storage.size());
        } else {
            validators.etag_ = source_value.precomputed_etag_;
        }
    }
    // Date preconditions also apply when response validators are not emitted.
    // An unrepresentable date is unavailable, not a truncated wire validator.
    if (has_response_time) {
        if (source_value.precomputed_last_modified_.empty() || !last_modified_is_actual) {
            if (const auto date = format_http_date(validators.modified_seconds_)) {
                last_modified_storage = *date;
                validators.last_modified_ =
                    std::string_view(last_modified_storage.data(), last_modified_storage.size());
            }
        } else {
            validators.last_modified_ = source_value.precomputed_last_modified_;
        }
    }
    return validators;
}

[[nodiscard]] http_representation_response_plan plan_file_representation(const context& context_value,
    const file_response_source& source_value, const file_validators& validators) {
    return plan_http_representation_response(detail::context_access::request(context_value),
        http_selected_representation_metadata{
            .length_ = source_value.size_,
            .etag_ = validators.etag_,
            .last_modified_ = validators.last_modified_.empty() ? std::nullopt
                                                                : std::optional(validators.modified_seconds_),
            // Filesystem mtime cannot establish that a representation changed
            // at most once during the second in its Last-Modified value.
            .strong_date_validator_ = false,
            .content_coded_ = source_value.content_coding_ != http_content_coding::identity,
        },
        http_representation_response_options{
            .normal_status_ = detail::context_access::response_status(context_value),
            .range_policy_ = source_value.range_requests_ == static_range_request_policy::honor
                                 ? http_range_request_policy::honor_byte_ranges
                                 : http_range_request_policy::ignore,
        });
}

// The validators and the RFC 9110 response one file response's request
// selects, decided from the source alone, before any file is opened. The
// validator views may borrow this object's storage, so it stays where it was
// built.
class file_response_plan final {
public:
    file_response_plan(const context& context_value, const file_response_source& source_value)
        : validators_(compute_file_validators(source_value, etag_storage_, last_modified_storage_)),
          representation_(plan_file_representation(context_value, source_value, validators_)),
          head_(detail::context_access::request(context_value).known_method() == http_known_method::head) {}

    file_response_plan(const file_response_plan&) = delete;
    file_response_plan& operator=(const file_response_plan&) = delete;
    file_response_plan(file_response_plan&&) = delete;
    file_response_plan& operator=(file_response_plan&&) = delete;

    [[nodiscard]] const file_validators& validators() const noexcept {
        return validators_;
    }

    [[nodiscard]] const http_representation_response_plan& representation() const noexcept {
        return representation_;
    }

    // Whether the response sends the file's bytes. A HEAD, 304, 412 or 416
    // only describes them, so no transport read ever checks the file behind
    // such a response; its open has to.
    [[nodiscard]] bool sends_file_bytes() const noexcept {
        return !head_ && representation_.precondition_failed() == nullptr &&
               representation_.not_modified() == nullptr && representation_.range_unsatisfiable() == nullptr;
    }

private:
    std::array<char, detail::static_file_etag_size> etag_storage_{};
    std::array<char, http_imf_fixdate_size> last_modified_storage_{};
    file_validators validators_;
    http_representation_response_plan representation_;
    bool head_;
};

// Which representation metadata a response repeats. RFC 9110 15.4.5: a 304
// carries the validators and cache-guiding fields a 200 would, but no other
// representation metadata; Last-Modified guides the cache update only when no
// ETag is sent. RFC 9110 15.3.7: a 206 answering a matched If-Range SHOULD NOT
// repeat representation fields beyond the required ones (ETag, Cache-Control,
// Content-Range or the multipart Content-Type), because the client already
// holds them. Content-Encoding stays: it names the coding of the bytes this
// message carries, and the response coding policy reads it to keep a
// precompressed variant acceptable to a client that refuses identity.
// RFC 9110 15.5.17: a 416 has no content, so Content-Type and Content-Encoding
// would describe bytes that are not sent; it keeps Content-Range and the
// validator and cache fields.
enum class file_header_set : std::uint8_t {
    full,
    if_range_partial,
    not_modified,
    range_unsatisfied,
};

void add_file_headers(http_response& response, const file_response_source& source_value,
    const file_validators& validators, file_header_set header_set = file_header_set::full,
    bool emit_content_type = true) {
    const bool full = header_set == file_header_set::full;
    const bool carries_content =
        header_set == file_header_set::full || header_set == file_header_set::if_range_partial;
    response.reserve_headers(file_response_header_reserve);
    if (full && emit_content_type) {
        response.header("Content-Type", source_value.content_type_);
    }
    if (!source_value.cache_control_.empty()) {
        response.header("Cache-Control", source_value.cache_control_);
    }
    // A precompressed variant carries the original Content-Type with the
    // encoding declared here.
    if (carries_content && source_value.content_coding_ != http_content_coding::identity) {
        response.header_stable_view(
            "Content-Encoding", http_content_coding_token(source_value.content_coding_));
    }
    if (header_set != file_header_set::not_modified &&
        source_value.range_requests_ == static_range_request_policy::honor) {
        response.header_stable_view("Accept-Ranges", "bytes");
    }
    if (source_value.response_validators_ == static_response_validator_policy::emit) {
        response.header("ETag", validators.etag_);
        if ((full || header_set == file_header_set::range_unsatisfied) && !validators.last_modified_.empty()) {
            response.header("Last-Modified", validators.last_modified_);
        }
    }
}

[[nodiscard]] file_header_set partial_file_header_set(const http_representation_response_plan& plan) noexcept {
    return plan.authorized_by_if_range() ? file_header_set::if_range_partial : file_header_set::full;
}

void apply_file_response_state(context& context_value, const file_response_source& source_value,
    http_response& response, std::optional<http_status_code> status_code) {
    detail::context_access::apply_response_state(context_value, response, status_code);
    // Declare the negotiation dimension after context response metadata is
    // applied. A caller-provided Vary value must be merged, not allowed to
    // overwrite Accept-Encoding and make differently encoded variants share
    // one cache entry (RFC 9110 12.5.5 / RFC 9111 4.1). context::file does no
    // Accept-Encoding negotiation and stays Vary-free.
    if (source_value.negotiates_encoding_) {
        response.add_vary_token("Accept-Encoding");
    }
}

[[nodiscard]] http_response make_not_modified_response(context& context_value,
    const file_response_source& source_value, const file_validators& validators, http_status_code status_code) {
    http_response response;
    add_file_headers(response, source_value, validators, file_header_set::not_modified);
    apply_file_response_state(context_value, source_value, response, status_code);
    return response;
}

[[nodiscard]] http_response make_full_file_response(context& context_value,
    const file_response_source& source_value, file_response_body_source& body, const file_validators& validators,
    std::optional<http_status_code> status_code) {
    http_response response;
    add_file_headers(response, source_value, validators);
    body.set_full_body(response, source_value.size_);
    apply_file_response_state(context_value, source_value, response, status_code);
    return response;
}

[[nodiscard]] http_response make_multipart_response(context& context_value,
    const file_response_source& source_value, file_response_body_source& body, const file_validators& validators,
    const http_byte_range_set& ranges, file_header_set header_set) {
    std::array<char, detail::secure_token_size> boundary_token;
    const auto token = detail::generate_secure_token(boundary_token);
    http_response response;
    add_file_headers(response, source_value, validators, header_set, false);
    apply_file_response_state(context_value, source_value, response, http_status::partial_content);
    const auto media_type = response.header("Content-Type");
    const auto selected_media_type = media_type.value_or(source_value.content_type_);
    auto multipart_plan = make_http_multipart_byte_range_plan(
        ranges, source_value.size_, selected_media_type, token);

    // The outer representation is the multipart body, which is not
    // content-coded; coded representations never reach this path.
    response.header("Content-Type", multipart_plan.content_type());
    response.remove_header("Content-Range");
    response.remove_header("Content-Encoding");
    response.remove_header("Content-Length");
    body.set_multipart_body(response, source_value.size_, std::move(multipart_plan));
    return response;
}

// Builds the response `planned` selected. An indexed file behind a response
// that sends none of its bytes was already checked by its open.
[[nodiscard]] http_response make_file_response(context& context_value, const file_response_source& source_value,
    file_response_body_source& body, const file_response_plan& planned) {
    const auto& validators = planned.validators();
    const auto& plan = planned.representation();
    if (plan.precondition_failed()) {
        throw http_error({.status_ = plan.status(),
            .code_ = "precondition_failed",
            .message_ = "file precondition failed"});
    }
    if (plan.not_modified()) {
        return make_not_modified_response(context_value, source_value, validators, plan.status());
    }
    if (plan.range_unsatisfiable()) {
        http_response response;
        response.content_range_unsatisfied(source_value.size_);
        add_file_headers(response, source_value, validators, file_header_set::range_unsatisfied);
        apply_file_response_state(context_value, source_value, response, plan.status());
        return response;
    }
    if (const auto* ranges = plan.multipart_ranges()) {
        return make_multipart_response(
            context_value, source_value, body, validators, *ranges, partial_file_header_set(plan));
    }
    if (const auto* range = plan.partial()) {
        http_response response;
        add_file_headers(response, source_value, validators, partial_file_header_set(plan));
        response.content_range(range->offset_, range->length_, source_value.size_);
        body.set_body(response, source_value.size_, range->offset_, range->length_);
        apply_file_response_state(context_value, source_value, response, plan.status());
        return response;
    }
    return make_full_file_response(context_value, source_value, body, validators, plan.status());
}

[[nodiscard]] http_error file_not_found_error() {
    return http_error({.status_ = ruvia::http_status::not_found,
        .code_ = "not_found",
        .message_ = "file not found"});
}

// context::file once its inputs are owned: `native_path` in the process
// resource, since it travels to the pool thread, and `content_type` in the
// request pool. An empty `content_type` selects the type guessed from the
// path.
[[nodiscard]] task<http_response> serve_file(
    context& context_value, http_native_path_string_type native_path, std::string content_type) {
    const auto content_type_value = content_type.empty() ? detail::guess_static_file_content_type_from_path_view(
                                                               http_native_path_view_type(native_path))
                                                         : std::string_view(content_type);
    // Open once, on the blocking pool: the response is framed from this
    // handle's metadata, and the body comes from the same handle, so no path
    // is resolved twice.
    auto opened = take_file_open(context_value,
        co_await run_file_open(context_value, [path = std::move(native_path)]() mutable {
            opened_file_path result{.path_ = std::move(path)};
            result.file_ = detail::open_response_file(result.path_.c_str(), result.error_);
            return result;
        }));
    if (opened.error_) {
        throw file_not_found_error();
    }
    const auto& snapshot = opened.file_.snapshot_;
    const file_response_source source_value{
        .size_ = snapshot.size_,
        .modified_token_ = snapshot.modified_token_,
        .modified_seconds_ = snapshot.modified_seconds_,
        .identity_ = snapshot.identity_,
        .content_type_ = content_type_value,
        .cache_control_ = {},
        .range_requests_ = static_range_request_policy::honor,
        .response_validators_ = static_response_validator_policy::emit,
        .precomputed_etag_ = {},
        .precomputed_last_modified_ = {},
        .content_coding_ = http_content_coding::identity,
        .negotiates_encoding_ = false,
    };
    const file_response_plan planned(context_value, source_value);
    auto body = file_response_body_source::file(
        file_response_path(opened.path_, snapshot.identity_, std::move(opened.file_.handle_)));
    auto response = make_file_response(context_value, source_value, body, planned);
    detail::context_access::discard_draft(context_value);
    co_return response;
}

// static_file once its representation is selected. `served` and `base_entry`
// view `root`'s snapshot, which the caller keeps alive until this completes;
// `content_type` is owned in the request pool, and an empty one selects the
// base entry's type.
[[nodiscard]] task<http_response> serve_static_file(context& context_value, const static_root& root,
    static_file_representation served, detail::static_root_entry_view base_entry, std::string content_type) {
    const auto& served_entry = served.entry();
    const auto* const memory_variant = served.memory_variant();
    // A document-root snapshot keeps small files' bytes; they describe exactly
    // the indexed metadata, so the response is framed and sent from memory.
    const auto* const resident_bytes = memory_variant == nullptr ? served_entry.resident_bytes() : nullptr;
    const bool file_backed = memory_variant == nullptr && resident_bytes == nullptr;
    const file_response_source source_value{
        .size_ = memory_variant == nullptr ? served_entry.size() : memory_variant->size(),
        .modified_token_ = memory_variant == nullptr ? served_entry.modified_token() : memory_variant->modified_token(),
        .modified_seconds_ =
            memory_variant == nullptr ? served_entry.modified_seconds() : memory_variant->modified_seconds(),
        .identity_ = file_backed ? served_entry.identity() : http_response_file_identity::unchecked(),
        .content_type_ = content_type.empty() ? base_entry.content_type() : std::string_view(content_type),
        .cache_control_ = base_entry.get_cache_control(),
        .range_requests_ = base_entry.range_requests(),
        .response_validators_ = base_entry.response_validators(),
        .precomputed_etag_ = memory_variant == nullptr ? served_entry.etag() : memory_variant->etag(),
        .precomputed_last_modified_ =
            memory_variant == nullptr ? served_entry.last_modified() : memory_variant->last_modified(),
        .content_coding_ = served.content_coding(),
        // static_file negotiates the representation by Accept-Encoding.
        .negotiates_encoding_ = true,
    };
    const file_response_plan planned(context_value, source_value);

    // The indexed file is opened beneath its root on the blocking pool. One
    // that cannot be opened is gone or no longer where the index found it,
    // and its path is never resolved another way. A response that sends none
    // of its bytes checks the opened handle against the index there too.
    http_response_file_descriptor opened;
    if (file_backed) {
        opened = take_file_open(context_value,
            co_await run_file_open(context_value,
                detail::static_root_access::entry_open(root, served_entry, !planned.sends_file_bytes())));
        if (!opened) {
            throw static_file_changed_error();
        }
    }
    auto body = memory_variant != nullptr ? file_response_body_source::bytes_value(memory_variant->bytes())
                : resident_bytes != nullptr
                    ? file_response_body_source::bytes_value(*resident_bytes)
                    : file_response_body_source::file(file_response_path(
                          http_native_path_view_type(served_entry.file_path()), served_entry.identity(),
                          std::move(opened)));
    auto response = make_file_response(context_value, source_value, body, planned);
    detail::context_access::discard_draft(context_value);
    co_return response;
}

}  // namespace

// Not a coroutine: the options are owned before the task is returned, so a
// stored task never reads the caller's path or content type.
task<http_response> context::file(file_response_options options) {
    http_native_path_string_type native_path;
    if (!assign_native_file_path(options.path_, native_path)) {
        throw file_not_found_error();
    }
    return serve_file(*this, std::move(native_path), std::string(options.content_type_.view()));
}

// Not a coroutine: the path is resolved and the content type owned before the
// task is returned; only `root` stays borrowed.
task<http_response> context::static_file(const static_root& root, static_file_response_options options) {
    // relative_path_ is a decoded file path -- a wildcard capture from
    // req().param("*") is passed as is -- so it is never percent-decoded here:
    // a second decode would turn "%252E%252E" into a ".." no route or path
    // scope ever saw. normalize_static_relative_path rejects "." and ".."
    // segments, absolute paths and NUL, and the lookup is a byte-exact
    // index compare that never joins the path onto the filesystem.
    std::string relative_storage;
    auto relative = options.relative_path_.view();
    const auto entry_value = detail::static_root_access::resolve(root, relative, relative_storage);
    if (!entry_value.has_value()) {
        throw file_not_found_error();
    }
    const auto mode = services().precompressed_static_files() ? detail::static_file_selection_mode::precompressed
                                                              : detail::static_file_selection_mode::identity_only;
    return detail::context_access::static_file_resolved(
        *this, root, relative, options.content_type_.view(), *entry_value, mode);
}

task<http_response> detail::context_access::static_file_resolved(
    context& context_value, const static_root& root, std::string_view relative,
    std::string_view content_type_value, detail::static_root_entry_view base_entry,
    detail::static_file_selection_mode mode) {
    // Serve a precompressed variant when the client accepts one; the bytes and
    // validators come from the variant, the Content-Type from the base entry.
    // Admission already folded Accept-Encoding; only a context created outside
    // admission negotiates from the request here.
    const auto served = [&]() -> std::optional<static_file_representation> {
        if (mode == detail::static_file_selection_mode::identity_only) {
            return static_file_representation(base_entry, http_content_coding::identity);
        }
        if (const auto* admitted = context_value.services().response_coding()) {
            return select_static_file_representation(root, relative, *admitted, base_entry);
        }
        return select_static_file_representation(
            root, relative, http_response_coding_selection_for(context_value.request_), base_entry);
    }();
    if (!served.has_value()) {
        throw http_error(detail::http_not_acceptable_coding_error());
    }
    return serve_static_file(
        context_value, root, *served, base_entry, std::string(content_type_value));
}

}  // namespace ruvia

namespace ruvia {

namespace detail {

// A media-type mismatch is the client speaking the wrong format at a valid
// endpoint: RFC 9110 15.5.16 assigns that 415, distinct from the 400 a
// malformed body of the RIGHT type earns below.
[[noreturn]] void throw_invalid_json_content_type() {
    throw http_error({.status_ = http_status::unsupported_media_type,
        .code_ = "unsupported_media_type",
        .message_ = "request body must be application/json"});
}

[[noreturn]] void throw_invalid_json_body() {
    throw http_error({.status_ = http_status::bad_request, .message_ = "invalid json body"});
}

[[noreturn]] void throw_invalid_form_content_type() {
    throw http_error({.status_ = http_status::unsupported_media_type,
        .code_ = "unsupported_media_type",
        .message_ = "request body must be application/x-www-form-urlencoded"});
}

[[noreturn]] void throw_invalid_query() {
    throw http_error({.status_ = http_status::bad_request, .message_ = "invalid query"});
}

[[noreturn]] void throw_invalid_param() {
    throw http_error({.status_ = http_status::bad_request, .message_ = "invalid route parameter"});
}

}  // namespace detail

namespace {

[[nodiscard]] const request_name_value_list& empty_request_fields() noexcept {
    static const request_name_value_list empty =
        detail::request_name_value_list_access::make();
    return empty;
}

// Unencoded names and values borrow the request query string; decoded ones
// live in the request arena. Form decoding keeps an invalid '%' literally.
void ensure_request_query(const http_request& request,
    detail::context_request_storage& request_storage) {
    auto& cache = request_storage.query_;
    if (cache) {
        return;
    }
    std::size_t pair_count = 0;
    std::size_t decoded_capacity = 0;
    (void)visit_url_encoded_pairs(request.query_string(), [&](std::string_view key, std::string_view value) {
        ++pair_count;
        if (has_url_encoding(key, url_decode_mode::form)) {
            decoded_capacity += key.size();
        }
        if (has_url_encoding(value, url_decode_mode::form)) {
            decoded_capacity += value.size();
        }
        return true;
    });
    auto& cached = cache.emplace();
    auto& decoded_bytes = cached.decoded_bytes();
    decoded_bytes.resize(decoded_capacity);
    std::size_t cursor = 0;
    const auto decode_form = [&](std::string_view input) noexcept -> std::string_view {
        if (!has_url_encoding(input, url_decode_mode::form)) {
            return input;
        }
        char* const out = decoded_bytes.data() + cursor;
        const auto size = *detail::decode_url_bytes(input, out, url_decode_mode::form, false);
        cursor += size;
        return std::string_view(out, size);
    };
    auto& query = cached.mutable_fields();
    detail::request_name_value_list_access::reserve(query, pair_count);
    (void)visit_url_encoded_pairs(request.query_string(), [&](std::string_view key, std::string_view value) {
        const auto name = decode_form(key);
        const auto decoded_value = decode_form(value);
        detail::request_name_value_list_access::push_back(query, http_header_view{name, decoded_value});
        return true;
    });
}

// Counts encoded captures only; decoding validates them once, in ensure_route_params.
[[nodiscard]] std::size_t count_encoded_route_params(const std::string_view* param_values,
    std::size_t param_count, detail::context_request_storage& request_storage) {
    if (request_storage.route_params_unencoded_) {
        return 0;
    }
    std::size_t encoded_count = 0;
    for (std::size_t i = 0; i < param_count; ++i) {
        encoded_count += has_url_encoding(param_values[i], url_decode_mode::percent);
    }
    if (encoded_count == 0) {
        request_storage.route_params_unencoded_ = true;
    }
    return encoded_count;
}

// Route names and unencoded captures borrow stable route/request storage; a
// "*" capture rejects escapes revealing a path separator, as static paths do.
void ensure_route_params(const std::string_view* param_names,
    const std::string_view* param_values, std::size_t param_count,
    detail::context_request_storage& request_storage) {
    auto& cache = request_storage.route_params_;
    if (cache) {
        return;
    }
    // A capture that failed to decode keeps failing without another decode.
    if (request_storage.route_params_invalid_) {
        detail::throw_invalid_param();
    }
    std::size_t decoded_capacity = 0;
    for (std::size_t i = 0; i < param_count; ++i) {
        if (has_url_encoding(param_values[i], url_decode_mode::percent)) {
            decoded_capacity += param_values[i].size();
        }
    }
    auto& decoded_bytes = request_storage.decoded_route_params_;
    decoded_bytes.clear();
    decoded_bytes.resize(decoded_capacity);
    std::size_t cursor = 0;
    auto params = detail::request_name_value_list_access::make();
    detail::request_name_value_list_access::reserve(params, param_count);
    for (std::size_t i = 0; i < param_count; ++i) {
        std::string_view decoded_value = param_values[i];
        if (has_url_encoding(decoded_value, url_decode_mode::percent)) {
            char* const out = decoded_bytes.data() + cursor;
            const auto size = detail::decode_url_bytes(
                decoded_value, out, url_decode_mode::percent, param_names[i] == "*");
            if (!size) {
                decoded_bytes.clear();
                request_storage.route_params_invalid_ = true;
                detail::throw_invalid_param();
            }
            decoded_value = std::string_view(out, *size);
            cursor += *size;
        }
        detail::request_name_value_list_access::push_back(params, http_header_view{param_names[i], decoded_value});
    }
    cache.emplace(std::move(params));
}

[[nodiscard]] std::string_view negotiation_header_name(context_request::negotiable_field field) noexcept {
    switch (field) {
        case context_request::negotiable_field::language:
            return "Accept-Language";
        case context_request::negotiable_field::encoding:
            return "Accept-Encoding";
        case context_request::negotiable_field::charset:
            return "Accept-Charset";
        case context_request::negotiable_field::media_type:
            break;
    }
    return "Accept";
}

[[nodiscard]] multipart_boundary get_multipart_boundary(const http_request& request) {
    const auto boundary = parse_multipart_boundary(
        request.header("Content-Type").value_or(std::string_view{}));
    if (const auto* parsed = boundary.boundary()) {
        return *parsed;
    }
    if (boundary.not_applicable() != nullptr) {
        throw http_error({.status_ = http_status::unsupported_media_type,
            .code_ = "unsupported_media_type",
            .message_ = "request body must be multipart/form-data"});
    }
    if (const auto* failure = boundary.failure()) {
        throw failure->protocol_error();
    }
    throw std::logic_error("unexpected multipart boundary parse result");
}

// Decoded bytes a request body may produce on the event loop. A body that
// decodes past it (a large body, or a few-KiB compression bomb) is decoded
// again on the worker's bounded blocking pool. Without a pool, or when the pool
// rejects the job, it is decoded inline: the handler requires the content, and
// the route's decoded-size cap still bounds the work.
inline constexpr std::size_t inline_request_decode_bytes = 64 * 1024;

[[nodiscard]] task<std::variant<std::string, http_content_decode_error>> decode_request_content(
    const worker_ref& worker_value, const stop_token& stop_token_value,
    std::span<const http_content_coding> codings, std::string_view raw,
    http_content_decode_options options) {
    if (worker_value.blocking_pool() != nullptr && options.max_decoded_bytes_ > inline_request_decode_bytes) {
        if (raw.size() <= inline_request_decode_bytes) {
            auto inline_result = decode_http_content(codings, raw,
                {.max_decoded_bytes_ = inline_request_decode_bytes});
            const auto* failure = std::get_if<http_content_decode_error>(&inline_result);
            if (failure == nullptr || *failure != http_content_decode_error::decoded_size_exceeded) {
                co_return inline_result;
            }
        }
        // The job carries only its own copies; request memory stays on this worker.
        std::array<http_content_coding, max_content_codings> owned_codings{};
        const auto coding_count = (std::min)(codings.size(), max_content_codings);
        std::copy_n(codings.data(), coding_count, owned_codings.data());
        std::string owned_raw(raw);
        auto offloaded = co_await run_blocking(worker_value, {.stop_token_ = stop_token_value},
            [owned_codings, coding_count, owned_raw = std::move(owned_raw),
                limit = options.max_decoded_bytes_]() {
                return decode_http_content(std::span<const http_content_coding>(owned_codings.data(), coding_count),
                    owned_raw, {.max_decoded_bytes_ = limit});
            });
        // A codec failure on the pool is rethrown here; pool saturation or
        // shutdown throws blocking_operation_rejected (mapped to 503).
        co_return std::move(offloaded).take_value();
    }
    co_return decode_http_content(codings, raw, options);
}

// Rejects an invalid or unsupported Content-Encoding; the result owns the codings span.
[[nodiscard]] http_content_coding_field_result require_decodable_request_coding(
    const http_request& request, detail::unsupported_request_content_coding::read_type read) {
    auto parsed_coding = request_content_coding(request);
    if (const auto* invalid = parsed_coding.invalid()) {
        throw http_protocol_error(
            invalid->status(), "invalid_content_encoding", "invalid request Content-Encoding");
    }
    if (parsed_coding.unsupported() != nullptr) {
        throw detail::unsupported_request_content_coding(read);
    }
    return parsed_coding;
}

task<std::string_view> read_request_text(const context& context_value, operation_options operation) {
    if (operation.stop_token_.stop_requested()) {
        detail::operation_interrupt::throw_cause(detail::operation_interrupt_cause::cancelled, "request body read");
    }
    if (const auto decoded = detail::context_access::decoded_body(context_value)) {
        co_return *decoded;
    }

    const auto& body_source = detail::context_access::request_body_source(context_value);
    const auto& request = detail::context_access::request(context_value);
    std::string_view raw;
    if (const auto* lazy = body_source.lazy()) {
        raw = co_await lazy->loader().read_all(std::move(operation));
    } else if (body_source.streaming() != nullptr) {
        throw std::logic_error("streaming request body cannot be buffered");
    } else {
        raw = as_chars(request.body_bytes());
    }
    if (detail::context_access::body_identity(context_value)) {
        co_return raw;
    }

    // Transparently decode a request body whose Content-Encoding we understand,
    // so handlers always see the decoded representation (RFC 9110 §8.4).
    const auto& services = detail::context_access::services(context_value);
    const auto parsed_coding = require_decodable_request_coding(
        request, detail::unsupported_request_content_coding::read_type::decoded);
    const auto codings = parsed_coding.codings();
    if (codings.empty()) {
        detail::context_access::mark_body_identity(context_value);
        co_return raw;
    }
    auto decode_result = co_await decode_request_content(services.worker(), context_value.stop_token(), codings, raw,
        {.max_decoded_bytes_ = services.max_decoded_request_body_bytes()});
    auto* decoded_content = std::get_if<std::string>(&decode_result);
    if (decoded_content == nullptr) {
        throw http_request_content_decode_error(std::get<http_content_decode_error>(decode_result));
    }
    co_return detail::context_access::store_decoded_body(context_value, std::move(*decoded_content));
}

task<std::span<const std::byte>> read_request_bytes(const context& context_value, operation_options operation) {
    const auto body = co_await read_request_text(context_value, std::move(operation));
    co_return std::as_bytes(std::span(body));
}

task<std::vector<multipart_part>> read_request_multipart(const context& context_value, operation_options operation) {
    const auto boundary = get_multipart_boundary(detail::context_access::request(context_value));
    const auto request_body = co_await read_request_text(context_value, std::move(operation));
    co_return parse_multipart_body(request_body, {.boundary_ = boundary});
}

task<void> discard_request_body(const context& context_value, operation_options operation) {
    if (operation.stop_token_.stop_requested()) {
        detail::operation_interrupt::throw_cause(detail::operation_interrupt_cause::cancelled, "request body read");
    }
    const auto& body_source = detail::context_access::request_body_source(context_value);
    if (const auto* lazy = body_source.lazy()) {
        co_await lazy->loader().discard(std::move(operation));
        co_return;
    }
    if (const auto* streaming = body_source.streaming()) {
        const detail::operation_steps steps(std::move(operation));
        while (co_await streaming->reader().read(steps.next("request body read"))) {
        }
    }
}

}  // namespace

std::string_view context_request::method() const noexcept {
    return context_->request_.method();
}

http_known_method context_request::known_method() const noexcept {
    return context_->request_.known_method();
}

std::string_view context_request::path() const noexcept {
    return context_->request_.path();
}

std::string_view context_request::scheme() const noexcept {
    return context_->request_.scheme();
}

std::string_view context_request::authority() const noexcept {
    return context_->request_.authority();
}

std::string_view context_request::target() const noexcept {
    return context_->request_.target();
}

http_protocol_version context_request::protocol_version() const noexcept {
    return context_->request_.protocol_version();
}

http_request_target_form context_request::target_form() const noexcept {
    return context_->request_.target_form();
}

std::string_view context_request::route_path() const noexcept {
    return context_->route_path_;
}

http_priority context_request::priority() const noexcept {
    if (const auto* update = context_->services().request_priority_update(); update != nullptr && *update) {
        return **update;
    }
    const auto parsed_value = parse_http_priority(context_->request_.headers());
    return (parsed_value.index() == 0) ? std::get<0>(parsed_value).request_priority() : http_priority{};
}

void context_response_editor::priority(http_priority_fields fields_value) {
    std::array<char, 12> value{};
    const auto encoded = encode_http_priority(value, fields_value);
    if ((encoded.index() != 0)) {
        throw std::invalid_argument("invalid HTTP priority parameters");
    }
    header("Priority", std::string_view(value.data(), std::get<0>(encoded)));
}

std::optional<std::string_view> context_request::header(std::string_view name) const {
    return context_->request_.header(name);
}

std::span<const http_header> context_request::trailers() const noexcept {
    return context_->services().request_trailers() != nullptr ? context_->services().request_trailers()->fields()
                                                              : std::span<const http_header>{};
}

std::optional<std::string_view> context_request::trailer(std::string_view name) const noexcept {
    return context_->services().request_trailers() != nullptr ? context_->services().request_trailers()->field(name)
                                                              : std::nullopt;
}

const request_name_value_list& context_request::header_fields() const {
    auto& cache = context_->request_storage().headers_;
    if (!cache) {
        cache.emplace(detail::request_name_value_list_access::borrow_headers(context_->request_.headers()));
    }
    return *cache;
}

const request_name_value_list& context_request::query_fields() const {
    if (context_->request_.query_string().empty()) {
        return empty_request_fields();
    }
    auto& storage = context_->request_storage();
    ensure_request_query(context_->request_, storage);
    return storage.query_->fields();
}

std::optional<std::string_view> context_request::query(std::string_view name) const {
    if (context_->request_.query_string().empty()) {
        return std::nullopt;
    }
    auto& storage = context_->request_storage();
    ensure_request_query(context_->request_, storage);
    return storage.query_->fields().get(name);
}

std::span<const std::string_view> context_request::queries(std::string_view name) const {
    if (context_->request_.query_string().empty()) {
        return {};
    }
    auto& storage = context_->request_storage();
    ensure_request_query(context_->request_, storage);
    return storage.query_->values().values(name);
}

std::optional<std::string_view> context_request::cookie(std::string_view name) const {
    if (const auto& cache = context_->request_storage().cookies_) {
        return cache->get(name);
    }
    return context_->request_.cookie(name);
}

const request_name_value_list& context_request::cookie_fields() const {
    auto& cache = context_->request_storage().cookies_;
    if (cache) {
        return *cache;
    }
    // Pool-backed growth returns replaced descriptor blocks immediately.
    // Keep the candidate private until every Cookie field has been visited.
    std::optional<request_name_value_list> cookies;
    for (const auto& header_field : context_->request_.headers()) {
        if (header_field.name().size() != 6U ||
            !http_ascii_equals_ignore_case(header_field.name(), "Cookie")) {
            continue;
        }
        if (!cookies) {
            cookies.emplace(detail::request_name_value_list_access::make());
        }
        http_visit_cookie_pairs(header_field.value(),
            [&cookies](std::string_view key, std::string_view value) {
                detail::request_name_value_list_access::push_back(
                    *cookies, http_header_view{key, value});
                return true;
            });
    }
    if (!cookies) {
        return empty_request_fields();
    }
    cache.emplace(std::move(*cookies));
    return *cache;
}

const request_name_value_list& context_request::param_fields() const {
    if (context_->param_count_ == 0) {
        return empty_request_fields();
    }
    auto& storage = context_->request_storage();
    ensure_route_params(context_->param_names_, context_->param_values_, context_->param_count_, storage);
    return *storage.route_params_;
}

std::optional<std::string_view> context_request::param(std::string_view name) const {
    if (context_->param_count_ == 0) {
        return std::nullopt;
    }
    auto& storage = context_->request_storage();
    if (storage.route_params_) {
        return storage.route_params_->get(name);
    }
    if (count_encoded_route_params(context_->param_values_, context_->param_count_, storage) == 0) {
        for (std::size_t i = context_->param_count_; i > 0; --i) {
            if (context_->param_names_[i - 1] == name) {
                return context_->param_values_[i - 1];
            }
        }
        return std::nullopt;
    }
    ensure_route_params(context_->param_names_, context_->param_values_, context_->param_count_, storage);
    return storage.route_params_->get(name);
}

std::optional<std::string_view> context_request::negotiate(
    negotiable_field field, std::span<const std::string_view> supported) const noexcept {
    if (supported.empty()) {
        return std::nullopt;
    }

    const bool media_type = field == negotiable_field::media_type;
    const bool encoding = field == negotiable_field::encoding;
    const auto header_name = negotiation_header_name(field);
    // Only Accept-Language needs prefix matching in the generic token parser.
    const auto token_mode = field == negotiable_field::language
                                ? http_accept_token_match_mode::language_prefix
                                : http_accept_token_match_mode::exact;

    static constexpr std::size_t inline_value_capacity = 8;
    std::array<std::string_view, inline_value_capacity> inline_values{};
    std::size_t matching_count = 0;
    bool saw_header = false;
    const auto headers = context_->request_.headers();
    for (const auto& header_field : headers) {
        if (!http_ascii_equals_ignore_case(header_field.name(), header_name)) {
            continue;
        }
        saw_header = true;
        if (header_field.value().empty()) {
            continue;
        }
        if (matching_count < inline_values.size()) {
            inline_values[matching_count] = header_field.value();
        }
        ++matching_count;
    }
    if (!saw_header) {
        return supported.front();
    }
    if (matching_count == 0 && !encoding) {
        return std::nullopt;
    }
    std::optional<std::string_view> best;
    int best_quality = 0;
    const bool use_inline_values = matching_count <= inline_values.size();
    for (const auto offered : supported) {
        http_accept_match match;
        http_accepted_encoding_quality encoding_quality;
        const auto accumulate = [&](std::string_view field_value) noexcept {
            if (media_type) {
                match.update_media_type(field_value, offered);
            } else if (encoding) {
                encoding_quality.update(field_value, offered);
            } else {
                match.update_token(field_value, offered, token_mode);
            }
        };
        if (use_inline_values) {
            for (std::size_t i = 0; i < matching_count; ++i) {
                accumulate(inline_values[i]);
            }
        } else {
            for (const auto& header_field : headers) {
                if (http_ascii_equals_ignore_case(header_field.name(), header_name) &&
                    !header_field.value().empty()) {
                    accumulate(header_field.value());
                }
            }
        }
        const auto quality = encoding ? encoding_quality.quality(http_ascii_equals_ignore_case(offered, "identity"))
                                      : match.quality();
        if (quality == 0) {
            continue;
        }
        // Strictly greater, so `supported` order breaks the client's ties and
        // reads as the server's own preference.
        if (quality > best_quality) {
            best_quality = quality;
            best = offered;
        }
    }

    // An empty Accept-Encoding accepts identity; other empty negotiation fields
    // match no representation.
    return best;
}

task<std::string_view> context_request::text(operation_options operation) const {
    return ::ruvia::make_scoped_operation(context_->operation_scope_, read_request_text(*context_, std::move(operation)));
}

task<std::span<const std::byte>> context_request::bytes(operation_options operation) const {
    return ::ruvia::make_scoped_operation(context_->operation_scope_, read_request_bytes(*context_, std::move(operation)));
}

std::optional<std::string_view> context_request::signed_cookie(
    signed_cookie_lookup_options options) const {
    const auto name = options.name_.view();
    const auto secret = options.secret_.view();
    const auto stored = cookie(name);
    if (!stored.has_value() || stored->size() <= detail::cookie_signature_size) {
        return std::nullopt;
    }
    const auto value_size = stored->size() - detail::cookie_signature_size - 1;
    if ((*stored)[value_size] != '.') {
        return std::nullopt;
    }
    const auto value = stored->substr(0, value_size);
    const auto signature = stored->substr(value_size + 1);
    char expected[detail::cookie_signature_size];
    detail::write_cookie_signature(expected, secret, name, value);
    if (!detail::constant_time_bytes_equal(signature, std::string_view(expected, sizeof(expected)))) {
        return std::nullopt;
    }
    return value;
}

task<std::vector<multipart_part>> context_request::multipart(operation_options operation) const {
    return ::ruvia::make_scoped_operation(
        context_->operation_scope_, read_request_multipart(*context_, std::move(operation)));
}

task<void> context_request::discard_body(operation_options operation) const {
    return ::ruvia::make_scoped_operation(context_->operation_scope_, discard_request_body(*context_, std::move(operation)));
}

body_reader& context_request::body_reader() const {
    const auto* streaming = context_->request_body_source().streaming();
    if (streaming == nullptr) {
        throw std::logic_error("request body is not streamable");
    }
    return streaming->reader();
}

multipart_reader context_request::multipart_reader() const {
    const auto boundary = get_multipart_boundary(context_->request_);
    // The reader parses raw body bytes as they arrive and decodes no content
    // coding, so only an identity body is accepted.
    const auto parsed_coding = require_decodable_request_coding(
        context_->request_, detail::unsupported_request_content_coding::read_type::streaming);
    if (std::ranges::any_of(parsed_coding.codings(),
            [](http_content_coding coding) { return coding != http_content_coding::identity; })) {
        throw detail::unsupported_request_content_coding(
            detail::unsupported_request_content_coding::read_type::streaming);
    }
    return ::ruvia::multipart_reader(body_reader(), {.boundary_ = boundary});
}
}  // namespace ruvia

namespace ruvia::detail {
class http_capsule_stream_state final {
public:
    http_capsule_stream_state(http_tunnel& tunnel, http_capsule_config config)
        : worker_(tunnel.worker_),
          server_(&tunnel),
          decoder_(config),
          config_(config),
          inbound_(tunnel.inbound_),
          payload_reservation_(inbound_) {}
    http_capsule_stream_state(http_client_tunnel&& tunnel, http_capsule_config config)
        : worker_(tunnel.response_.state_->memory_domain().worker()),
          memory_pin_(http_client_response(tunnel.response_.state_)),
          client_(std::move(tunnel)),
          decoder_(config),
          config_(config) {}
    void require() const {
        if (closed_ || (client_ && (!client_->response_.state_ || client_->response_.state_->pool_ == nullptr))) {
            throw std::logic_error("capsule stream is closed");
        }
        if (!worker_.is_current()) {
            throw std::logic_error("capsule stream requires its owner worker");
        }
    }
    [[nodiscard]] operation_lane_lease claim_lane(bool output) {
        require();
        return claim_operation_lane(output ? output_active_ : read_active_, "capsule stream operation is already active");
    }
    void retain() noexcept {
        ++references_;
    }
    void release() noexcept {
        if (!worker_.is_current()) {
            std::terminate();
        }
        if (--references_ != 0) {
            return;
        }
        auto pin = std::move(memory_pin_);
        delete this;
    }
    void abort() noexcept {
        abort_input(false);
    }
    void abort_malformed() noexcept {
        abort_input(true);
    }
    void abort_input(bool malformed) noexcept {
        if (closed_) {
            return;
        }
        if (client_ && (!client_->response_.state_ || client_->response_.state_->pool_ == nullptr)) {
            closed_ = true;
            return;
        }
        if (!worker_.is_current()) {
            std::terminate();
        }
        closed_ = true;
        if (server_) {
            server_->abort_input(malformed);
        } else if (client_) {
            if (malformed) {
                client_->abort_malformed();
            } else {
                client_->abort();
            }
        }
    }
    void clear_input() noexcept {
        input_ = {};
        owned_input_.reset();
        offset_ = 0;
        native_input_ = false;
    }
    void close() noexcept {
        if (!worker_.is_current()) {
            std::terminate();
        }
        if (read_active_ || output_active_) {
            std::terminate();
        }
        if (client_) {
            abort();
        } else {
            closed_ = true;
        }
        read_scope_.close();
        output_scope_.close();
        clear_input();
        server_ = nullptr;
    }
    [[nodiscard]] bool poll_capsule(bool fin, http_capsule_retention retention) {
        const auto collect = [](void* raw, http_capsule_event event) {
            auto& state = *static_cast<http_capsule_stream_state*>(raw);
            state.type_ = event.type_;
            if (!event.payload_.empty()) {
                state.payload_.append(event.payload_.data(), event.payload_.size());
            }
        };
        const auto bytes_value = std::span<const char>(input_).subspan(offset_);
        const auto result_value = decoder_.feed_one(bytes_value, fin, collect, this, retention);
        offset_ += result_value.consumed_bytes_;
        if (result_value.status_ == http_capsule_status::limit || result_value.status_ == http_capsule_status::truncated) {
            abort_malformed();
            throw std::runtime_error("invalid or oversized capsule stream");
        }
        if (result_value.status_ == http_capsule_status::end) {
            receive_ended_ = true;
        }
        return result_value.capsule_complete_;
    }
    // Interruptions are std::system_error (timed_out / operation_canceled);
    // a client tunnel reports them as its http_client_error subclass.
    [[noreturn]] void throw_interrupted(operation_interrupt_cause cause, const char* operation_name) const {
        if (client_) {
            throw http_client_error(std::make_error_code(cause == operation_interrupt_cause::timed_out
                                                             ? std::errc::timed_out
                                                             : std::errc::operation_canceled),
                operation_name);
        }
        operation_interrupt::throw_cause(cause, operation_name);
    }
    // A stop requested before an operation starts leaves the stream untouched.
    void throw_if_stopped_before_start(const operation_options& operation, const char* operation_name) const {
        if (operation.stop_token_.stop_requested()) {
            throw_interrupted(operation_interrupt_cause::cancelled, operation_name);
        }
    }
    [[nodiscard]] operation_options next_step(const operation_steps& steps, const char* operation_name) const {
        auto step = steps.try_next();
        if (!step.has_value()) {
            throw_interrupted(operation_interrupt_cause::timed_out, operation_name);
        }
        return std::move(*step);
    }
    // Whether the exception being handled is a step interrupted by `steps`.
    [[nodiscard]] static bool interrupted(const operation_steps& steps) noexcept {
        try {
            throw;
        } catch (const std::system_error& error) {
            return steps.interrupted(error);
        } catch (...) {
            return false;
        }
    }
    [[nodiscard]] task<bool> read_input(bool datagrams, const operation_steps& steps) {
        clear_input();
        if (datagrams && has_native_transport()) {
            std::optional<http_tunnel_input> chunk;
            if (server_) {
                chunk = co_await server_->read_datagram_raw(next_step(steps, "capsule read"));
            }
#ifdef RUVIA_ENABLE_HTTP3
            else {
                auto& state = *client_->response_.state_;
                chunk = co_await state.pool_->read_http3_tunnel_input(state, next_step(steps, "capsule read"));
            }
#endif
            if (!chunk) {
                co_return false;
            }
            native_input_ = chunk->datagram_;
            owned_input_.emplace(std::move(chunk->bytes_));
            input_ = *owned_input_;
            co_return true;
        }
        if (server_) {
            auto chunk = co_await server_->read_raw(next_step(steps, "capsule read"));
            if (!chunk) {
                co_return false;
            }
            owned_input_.emplace(std::move(*chunk));
            input_ = *owned_input_;
        } else {
            auto chunk = co_await client_->read(next_step(steps, "capsule read"));
            if (!chunk) {
                co_return false;
            }
            input_ = as_chars(*chunk);
        }
        co_return true;
    }
    [[nodiscard]] bool has_native_transport() const noexcept {
        if (server_) {
            return server_->read_datagram_ != nullptr && server_->write_datagram_ != nullptr;
        }
#ifdef RUVIA_ENABLE_HTTP3
        return client_ && client_->response_.state_->transport_ == http_client_response_transport::http3;
#else
        return false;
#endif
    }
    [[nodiscard]] task<bool> send_datagram(std::string_view bytes, const operation_steps& steps) {
        if (server_) {
            co_return co_await server_->write_datagram_raw(bytes, next_step(steps, "capsule write"));
        }
#ifdef RUVIA_ENABLE_HTTP3
        auto& state = *client_->response_.state_;
        co_return co_await state.pool_->send_http3_datagram(state, bytes, next_step(steps, "capsule write"));
#else
        co_return false;
#endif
    }
    [[nodiscard]] task<void> send_bytes(std::string&& bytes_value, const operation_steps& steps) {
        if (server_ != nullptr) {
            co_await server_->write_raw(std::move(bytes_value), next_step(steps, "capsule write"));
            co_return;
        }
        const auto limit = client_->response_.state_->tunnel_->config_.max_chunk_bytes_;
        if (bytes_value.size() <= limit) {
            co_await client_->write_owned(std::move(bytes_value), next_step(steps, "capsule write"));
            co_return;
        }
        std::string_view remaining = bytes_value;
        while (!remaining.empty()) {
            const auto chunk = remaining.substr(0, limit);
            co_await client_->write(chunk, next_step(steps, "capsule write"));
            remaining.remove_prefix(chunk.size());
        }
    }
    const worker_ref& worker_;
    std::optional<http_client_response> memory_pin_;
    http_tunnel* server_{};
    std::optional<http_client_tunnel> client_;
    http_capsule_decoder decoder_;
    http_capsule_config config_;
    std::optional<std::string> owned_input_;
    std::string_view input_;
    std::size_t offset_{};
    bool native_input_{};
    inbound_buffer_account* inbound_{nullptr};
    std::string payload_;
    inbound_buffer_reservation payload_reservation_{};
    std::uint64_t type_{};
    bool receive_ended_{};
    bool send_ended_{};
    bool closed_{};
    bool read_active_{};
    bool output_active_{};
    std::size_t references_{1};
    ::ruvia::operation_scope read_scope_;
    ::ruvia::operation_scope output_scope_;
};
class capsule_state_pin final {
public:
    explicit capsule_state_pin(http_capsule_stream_state& state_value) noexcept
        : state_(&state_value) {
        state_->retain();
    }
    capsule_state_pin(capsule_state_pin&& other) noexcept
        : state_(std::exchange(other.state_, nullptr)) {}
    capsule_state_pin(const capsule_state_pin&) = delete;
    ~capsule_state_pin() {
        if (state_) {
            state_->release();
        }
    }
    [[nodiscard]] http_capsule_stream_state& state() const noexcept {
        return *state_;
    }

private:
    http_capsule_stream_state* state_;
};
struct capsule_write_input final {
    capsule_state_pin pin_;
    std::string bytes_;
    bool datagram_{};
    operation_options operation_;
};
// Failure aborts the stream, except an interrupted read, which keeps the
// partly received capsule for the next read.
class running_capsule_operation final {
public:
    running_capsule_operation(http_capsule_stream_state& state_value, bool output)
        : state_(state_value),
          exceptions_(std::uncaught_exceptions()),
          lane_(state_value.claim_lane(output)) {}
    ~running_capsule_operation() {
        if (aborts_ && std::uncaught_exceptions() > exceptions_) {
            state_.abort();
        }
    }
    void keep_stream() noexcept {
        aborts_ = false;
    }

private:
    http_capsule_stream_state& state_;
    int exceptions_;
    bool aborts_{true};
    operation_lane_lease lane_;
};
}  // namespace ruvia::detail

namespace ruvia {
namespace {
constexpr const char* tunnel_lane_in_progress = "HTTP tunnel operation already in progress on this direction";
template <typename input_type>
task<std::optional<input_type>> read_tunnel(void* target,
    task<std::optional<input_type>> (*read)(void*, operation_options), bool& read_active, operation_options operation) {
    auto lease = detail::claim_operation_lane(read_active, tunnel_lane_in_progress);
    co_return co_await read(target, std::move(operation));
}
task<void> write_tunnel(void* target, task<void> (*write)(void*, std::string_view, operation_options),
    std::string bytes_value, bool& write_active, bool& ended, operation_options operation) {
    auto lease = detail::claim_operation_lane(write_active, tunnel_lane_in_progress);
    if (ended) {
        throw std::logic_error("HTTP tunnel send direction has ended");
    }
    co_await write(target, bytes_value, std::move(operation));
}
task<bool> write_tunnel_datagram(void* target, task<bool> (*write)(void*, std::string_view, operation_options),
    std::string_view bytes, bool& write_active, bool& ended, operation_options operation) {
    auto lease = detail::claim_operation_lane(write_active, tunnel_lane_in_progress);
    if (ended) {
        throw std::logic_error("HTTP tunnel send direction has ended");
    }
    co_return co_await write(target, bytes, std::move(operation));
}
task<void> finish_tunnel(void* target, task<void> (*finish_value)(void*, operation_options),
    bool& ended, bool& write_active, operation_options operation) {
    auto lease = detail::claim_operation_lane(write_active, tunnel_lane_in_progress);
    if (!ended) {
        co_await finish_value(target, std::move(operation));
        ended = true;
    }
}
}  // namespace

http_tunnel::http_tunnel(detail::inbound_buffer_account* inbound, const worker_ref& worker_value,
    void* target, read_type read, write_type write, finish_type finish_value, abort_type abort,
    bool datagrams, read_datagram_type read_datagram, write_datagram_type write_datagram) noexcept
    : inbound_(inbound),
      worker_(worker_value),
      target_(target),
      read_(read),
      write_(write),
      finish_(finish_value),
      abort_(abort),
      read_datagram_(read_datagram),
      write_datagram_(write_datagram),
      datagrams_(datagrams) {
    operations_.bind_worker(worker_);
}

void http_tunnel::require_active() const {
    if (!worker_.is_current()) {
        std::terminate();
    }
    if (!operations_.active()) {
        throw std::logic_error("HTTP tunnel session has expired");
    }
}
void http_tunnel::require_unadapted() const {
    if (adapted_) {
        throw std::logic_error("HTTP tunnel is already adapted to a capsule stream");
    }
}
task<std::optional<std::span<const std::byte>>> http_tunnel::read(operation_options operation) & {
    require_active();
    require_unadapted();
    return ::ruvia::make_scoped_operation(operations_, read_borrowed(std::move(operation)));
}
task<std::optional<std::span<const std::byte>>> http_tunnel::read_borrowed(operation_options operation) {
    auto lease = detail::claim_operation_lane(read_active_, tunnel_lane_in_progress);
    // The previous view expires as this read starts.
    read_chunk_.reset();
    const detail::operation_steps steps(std::move(operation));
    for (;;) {
        auto chunk = co_await read_(target_, steps.next("HTTP tunnel read"));
        if (!chunk) {
            co_return std::nullopt;
        }
        if (!chunk->empty()) {
            read_chunk_.emplace(std::move(*chunk));
            co_return as_bytes(std::string_view(*read_chunk_));
        }
    }
}
task<std::optional<std::string>> http_tunnel::read_raw(operation_options operation) {
    require_active();
    return ::ruvia::make_scoped_operation(operations_, read_tunnel(target_, read_, read_active_, std::move(operation)));
}
task<std::optional<detail::http_tunnel_input>> http_tunnel::read_datagram_raw(operation_options operation) {
    require_active();
    return ::ruvia::make_scoped_operation(
        operations_, read_tunnel(target_, read_datagram_, read_active_, std::move(operation)));
}
task<bool> http_tunnel::write_datagram_raw(std::string_view bytes, operation_options operation) {
    require_active();
    return ::ruvia::make_scoped_operation(operations_,
        write_tunnel_datagram(target_, write_datagram_, bytes, write_active_, send_ended_, std::move(operation)));
}
task<void> http_tunnel::write(std::string_view bytes_value, operation_options operation) & {
    require_active();
    require_unadapted();
    if (send_ended_) {
        throw std::logic_error("HTTP tunnel send direction has ended");
    }
    return write_raw(std::string(bytes_value), std::move(operation));
}
task<void> http_tunnel::write(std::span<const std::byte> bytes_value, operation_options operation) & {
    return write(as_chars(bytes_value), std::move(operation));
}
task<void> http_tunnel::write_owned(std::string&& bytes_value, operation_options operation) {
    require_active();
    require_unadapted();
    return write_raw(std::move(bytes_value), std::move(operation));
}
task<void> http_tunnel::write_raw(std::string&& bytes_value, operation_options operation) {
    require_active();
    if (send_ended_) {
        throw std::logic_error("HTTP tunnel send direction has ended");
    }
    auto owned = std::move(bytes_value);
    return ::ruvia::make_scoped_operation(operations_,
        write_tunnel(target_, write_, std::move(owned), write_active_, send_ended_, std::move(operation)));
}
task<void> http_tunnel::finish(operation_options operation) & {
    require_active();
    return ::ruvia::make_scoped_operation(
        operations_, finish_tunnel(target_, finish_, send_ended_, write_active_, std::move(operation)));
}
void http_tunnel::abort() & noexcept {
    abort_input(false);
}
void http_tunnel::abort_input(bool malformed) noexcept {
    if (!worker_.is_current()) {
        std::terminate();
    }
    if (operations_.active()) {
        abort_(target_, malformed);
    }
}

http_capsule::http_capsule(detail::http_capsule_stream_state& state_value, std::uint64_t type, std::string payload_value) noexcept
    : state_(&state_value),
      type_(type),
      payload_(std::move(payload_value)) {
    state_->retain();
}
http_capsule::http_capsule(http_capsule&& other) noexcept
    : state_(std::exchange(other.state_, nullptr)),
      type_(other.type_),
      payload_(std::move(other.payload_)) {
    other.payload_.reset();
}
http_capsule& http_capsule::operator=(http_capsule&& other) noexcept {
    if (this != &other) {
        release();
        state_ = std::exchange(other.state_, nullptr);
        type_ = other.type_;
        // Reconstruct rather than propagate the previous capsule's allocator.
        if (other.payload_) {
            payload_.emplace(std::move(*other.payload_));
            other.payload_.reset();
        }
    }
    return *this;
}
http_capsule::~http_capsule() {
    release();
}
void http_capsule::release() noexcept {
    // Payload storage is returned before its domain pin can retire the pool.
    if (state_ && !state_->worker_.is_current()) {
        std::terminate();
    }
    payload_.reset();
    if (auto* state = std::exchange(state_, nullptr)) {
        state->release();
    }
}
http_capsule_stream::http_capsule_stream(http_tunnel& tunnel, http_capsule_config config) {
    tunnel.require_active();
    tunnel.require_unadapted();
    state_ = new detail::http_capsule_stream_state(tunnel, config);
    tunnel.adapted_ = true;
}
http_capsule_stream::http_capsule_stream(http_client_tunnel&& tunnel, http_capsule_config config) {
    if (!tunnel.response_.state_ || !tunnel.response_.state_->memory_domain().worker().is_current()) {
        throw std::logic_error("capsule stream requires a live tunnel on its worker");
    }
    // Validate before consuming the caller's owner.
    http_capsule_decoder validation(config);
    state_ = new detail::http_capsule_stream_state(std::move(tunnel), config);
}
http_capsule_stream::http_capsule_stream(http_capsule_stream&& other) noexcept
    : state_(std::exchange(other.state_, nullptr)) {}
http_capsule_stream& http_capsule_stream::operator=(http_capsule_stream&& other) noexcept {
    if (this != &other) {
        release();
        state_ = std::exchange(other.state_, nullptr);
    }
    return *this;
}
http_capsule_stream::~http_capsule_stream() {
    release();
}
void http_capsule_stream::release() noexcept {
    if (auto* state = std::exchange(state_, nullptr)) {
        state->close();
        state->release();
    }
}
void http_capsule_stream::abort() & noexcept {
    if (state_) {
        state_->abort();
    }
}
// datagrams() and udp() build on this one gate.
http_capsule_stream http_tunnel::capsules(http_capsule_config config) & {
    // The route protocol owns Capsule Protocol negotiation (RFC 9297 section
    // 3.4); a route that did not declare datagrams never advertised it.
    if (!datagrams_) {
        throw std::logic_error("HTTP tunnel route did not declare datagrams");
    }
    require_unadapted();
    return http_capsule_stream(*this, config);
}
http_capsule_stream http_client_tunnel::capsules(http_capsule_config config) && {
    return http_capsule_stream(std::move(*this), config);
}
task<std::optional<http_capsule>> http_capsule_stream::read(operation_options operation) & {
    if (!state_) {
        throw std::logic_error("capsule stream is empty");
    }
    state_->require();
    return ::ruvia::make_scoped_operation(
        state_->read_scope_, read_owned(detail::capsule_state_pin(*state_), std::move(operation)));
}
task<std::optional<http_capsule>> http_capsule_stream::read_owned(detail::capsule_state_pin pin, operation_options operation) {
    auto& state_value = pin.state();
    state_value.throw_if_stopped_before_start(operation, "capsule read");
    const detail::operation_steps steps(std::move(operation));
    detail::running_capsule_operation running(state_value, false);
    if (state_value.receive_ended_) {
        co_return std::nullopt;
    }
    try {
        for (;;) {
            bool fin{};
            if (state_value.offset_ == state_value.input_.size()) {
                fin = !co_await state_value.read_input(false, steps);
            }
            if (state_value.poll_capsule(fin, http_capsule_retention::all)) {
                auto payload_value = std::move(state_value.payload_);
                state_value.payload_ = std::string{};
                co_return http_capsule(state_value, state_value.type_, std::move(payload_value));
            }
            if (state_value.receive_ended_) {
                co_return std::nullopt;
            }
        }
    } catch (...) {
        if (detail::http_capsule_stream_state::interrupted(steps)) {
            running.keep_stream();
        }
        throw;
    }
}
task<void> http_capsule_stream::write(std::uint64_t type, std::span<const std::byte> payload_value, operation_options operation) & {
    return write(type, std::string_view(reinterpret_cast<const char*>(payload_value.data()), payload_value.size()),
        std::move(operation));
}
task<void> http_capsule_stream::write(std::uint64_t type, std::string_view payload_value, operation_options operation) & {
    if (!state_) {
        throw std::logic_error("capsule stream is empty");
    }
    state_->require();
    if (state_->send_ended_) {
        throw std::logic_error("capsule sending direction is closed");
    }
    std::array<char, 16> header;
    const auto encoded = encode_http_capsule_header(header, type, payload_value.size());
    if ((encoded.index() != 0) || payload_value.size() > state_->config_.max_capsule_length_) {
        throw std::length_error("invalid capsule type or payload length");
    }
    return write_frame(std::string_view(header.data(), std::get<0>(encoded)), payload_value, false, std::move(operation));
}
task<void> http_capsule_stream::write_owned(
    std::uint64_t type, std::string&& payload_value, operation_options operation) {
    if (!state_) {
        throw std::logic_error("capsule stream is empty");
    }
    state_->require();
    if (state_->send_ended_) {
        throw std::logic_error("capsule sending direction is closed");
    }
    std::array<char, 16> header;
    const auto encoded = encode_http_capsule_header(header, type, payload_value.size());
    if ((encoded.index() != 0) || payload_value.size() > state_->config_.max_capsule_length_) {
        throw std::length_error("invalid capsule type or payload length");
    }
    return write_frame_owned(std::string_view(header.data(), std::get<0>(encoded)),
        std::move(payload_value), false, std::move(operation));
}
task<void> http_capsule_stream::write_frame(
    std::string_view prefix, std::string_view payload_value, bool datagram, operation_options operation) {
    const auto write = +[](detail::capsule_write_input input) -> task<void> {
        auto& state_value = input.pin_.state();
        state_value.throw_if_stopped_before_start(input.operation_, "capsule write");
        const detail::operation_steps steps(std::move(input.operation_));
        detail::running_capsule_operation running(state_value, true);
        if (state_value.send_ended_) {
            throw std::logic_error("capsule sending direction is closed");
        }
        try {
            if (input.datagram_) {
                if (co_await state_value.send_datagram(input.bytes_, steps)) {
                    co_return;
                }
                std::array<char, 16> header;
                const auto encoded = encode_http_capsule_header(header, http_datagram_capsule_type, input.bytes_.size());
                if (encoded.index() != 0) {
                    throw std::length_error("HTTP Datagram exceeds capsule bound");
                }
                input.bytes_.insert(0, header.data(), std::get<0>(encoded));
            }
            co_await state_value.send_bytes(std::move(input.bytes_), steps);
        } catch (...) {
            state_value.abort();
            throw;
        }
    };
    std::string bytes_value;
    bytes_value.reserve(prefix.size() + payload_value.size());
    bytes_value.append(prefix);
    bytes_value.append(payload_value);
    return ::ruvia::make_scoped_operation(state_->output_scope_,
        write(detail::capsule_write_input{detail::capsule_state_pin(*state_), std::move(bytes_value), datagram, std::move(operation)}));
}
task<void> http_capsule_stream::write_frame_owned(
    std::string_view prefix, std::string&& payload_value, bool datagram, operation_options operation) {
    if (prefix.empty()) {
        const auto write = +[](detail::capsule_write_input input) -> task<void> {
            auto& state_value = input.pin_.state();
            state_value.throw_if_stopped_before_start(input.operation_, "capsule write");
            const detail::operation_steps steps(std::move(input.operation_));
            detail::running_capsule_operation running(state_value, true);
            if (state_value.send_ended_) {
                throw std::logic_error("capsule sending direction is closed");
            }
            try {
                if (input.datagram_) {
                    if (co_await state_value.send_datagram(input.bytes_, steps)) {
                        co_return;
                    }
                    std::array<char, 16> header;
                    const auto encoded = encode_http_capsule_header(header, http_datagram_capsule_type, input.bytes_.size());
                    if (encoded.index() != 0) {
                        throw std::length_error("HTTP Datagram exceeds capsule bound");
                    }
                    input.bytes_.insert(0, header.data(), std::get<0>(encoded));
                }
                co_await state_value.send_bytes(std::move(input.bytes_), steps);
            } catch (...) {
                state_value.abort();
                throw;
            }
        };
        auto owned = std::move(payload_value);
        return ::ruvia::make_scoped_operation(state_->output_scope_,
            write(detail::capsule_write_input{detail::capsule_state_pin(*state_), std::move(owned), datagram, std::move(operation)}));
    }
    return write_frame(prefix, payload_value, datagram, std::move(operation));
}
task<void> http_capsule_stream::finish(operation_options operation) & {
    if (!state_) {
        throw std::logic_error("capsule stream is empty");
    }
    state_->require();
    const auto finish_value = +[](detail::capsule_state_pin pin, operation_options finish_operation) -> task<void> {
        auto& state_value = pin.state();
        state_value.throw_if_stopped_before_start(finish_operation, "capsule finish");
        detail::running_capsule_operation running(state_value, true);
        if (!state_value.send_ended_) {
            if (state_value.server_) {
                co_await state_value.server_->finish(std::move(finish_operation));
            } else {
                co_await state_value.client_->finish(std::move(finish_operation));
            }
            state_value.send_ended_ = true;
        }
    };
    return ::ruvia::make_scoped_operation(
        state_->output_scope_, finish_value(detail::capsule_state_pin(*state_), std::move(operation)));
}

http_datagram_stream::http_datagram_stream(http_capsule_stream&& stream)
    : stream_(std::move(stream)) {
    if (!stream_.state_) {
        throw std::logic_error("HTTP Datagram stream is empty");
    }
}
http_datagram_stream http_tunnel::datagrams(http_capsule_config config) & {
    return http_datagram_stream(capsules(config));
}
http_udp_tunnel http_tunnel::udp(http_capsule_config config) & {
    return http_udp_tunnel(datagrams(config));
}
http_datagram_stream http_client_tunnel::datagrams(http_capsule_config config) && {
    if (!response_.state_ || !response_.state_->tunnel_ || !response_.state_->tunnel_->udp_ || !response_.state_->tunnel_->accepted_) {
        throw std::logic_error("HTTP Datagrams require an accepted datagram tunnel");
    }
    return http_datagram_stream(std::move(*this).capsules(config));
}
task<std::optional<http_datagram>> http_datagram_stream::read(operation_options operation) & {
    if (!stream_.state_) {
        throw std::logic_error("HTTP Datagram stream is empty");
    }
    stream_.state_->require();
    return ::ruvia::make_scoped_operation(
        stream_.state_->read_scope_, read_owned(detail::capsule_state_pin(*stream_.state_), false, std::move(operation)));
}
task<std::optional<http_datagram>> http_datagram_stream::read_owned(
    detail::capsule_state_pin pin, bool udp, operation_options operation) {
    auto& state_value = pin.state();
    state_value.throw_if_stopped_before_start(operation, "HTTP Datagram read");
    const detail::operation_steps steps(std::move(operation));
    detail::running_capsule_operation running(state_value, false);
    try {
        for (;;) {
            if (state_value.receive_ended_) {
                co_return std::nullopt;
            }
            bool fin{};
            if (state_value.offset_ == state_value.input_.size()) {
                fin = !co_await state_value.read_input(true, steps);
            }
            // The decoder drops other capsule types and, for CONNECT-UDP, unknown
            // Context IDs before the length limit applies (RFC 9297 section 3.2).
            if (!state_value.native_input_ && !state_value.poll_capsule(
                                                  fin, udp ? http_capsule_retention::udp_payloads : http_capsule_retention::datagrams)) {
                continue;
            }
            const bool native = state_value.native_input_;
            auto& owned = native ? *state_value.owned_input_ : state_value.payload_;
            if (native) {
                state_value.offset_ = state_value.input_.size();
            }
            auto bytes_value = std::span<const char>(owned);
            http_datagram_session datagrams;
            std::size_t offset{};
            if (udp) {
                auto decoded = datagrams.receive_udp_datagram(bytes_value);
                if ((decoded.index() != 0)) {
                    state_value.abort_malformed();
                    throw std::runtime_error("malformed CONNECT-UDP datagram");
                }
                if (!std::get<0>(decoded)) {
                    if (!native) {
                        owned.clear();
                    }
                    continue;
                }
                offset = bytes_value.size() - (std::get<0>(decoded))->payload_.size();
            } else {
                auto decoded = datagrams.receive_datagram(bytes_value);
                if (!decoded) {
                    if (!native) {
                        owned.clear();
                    }
                    continue;
                }
                offset = bytes_value.size() - decoded->size();
            }
            if (native && owned.size() > state_value.config_.max_capsule_length_) {
                state_value.abort_malformed();
                throw std::runtime_error("oversized HTTP Datagram");
            }
            auto result_value = std::move(owned);
            if (native) {
                state_value.clear_input();
            } else {
                state_value.payload_ = std::string{};
            }
            co_return http_datagram(http_capsule(state_value, http_datagram_capsule_type, std::move(result_value)), offset);
        }
    } catch (...) {
        if (detail::http_capsule_stream_state::interrupted(steps)) {
            running.keep_stream();
        }
        throw;
    }
}
task<void> http_datagram_stream::send(std::span<const std::byte> payload_value, operation_options operation) & {
    return send(std::string_view(reinterpret_cast<const char*>(payload_value.data()), payload_value.size()),
        std::move(operation));
}
task<void> http_datagram_stream::send(std::string_view payload_value, operation_options operation) & {
    return send_payload(payload_value, false, std::move(operation));
}
task<void> http_datagram_stream::send_owned(
    std::string&& payload_value, operation_options operation) {
    return send_payload_owned(std::move(payload_value), false, std::move(operation));
}
task<void> http_datagram_stream::send_payload(std::string_view payload_value, bool udp, operation_options operation) {
    if (!stream_.state_) {
        throw std::logic_error("HTTP Datagram stream is empty");
    }
    auto& state_value = *stream_.state_;
    state_value.require();
    if (state_value.send_ended_) {
        throw std::logic_error("HTTP Datagram sending direction is closed");
    }
    const auto context_bytes = udp ? std::size_t{1} : std::size_t{0};
    if (context_bytes > state_value.config_.max_capsule_length_ ||
        payload_value.size() > state_value.config_.max_capsule_length_ - context_bytes) {
        throw std::length_error("HTTP Datagram exceeds capsule bound");
    }
    http_datagram_session session_value;
    const auto plan = udp ? session_value.prepare_udp_datagram(std::span(payload_value.data(), payload_value.size()))
                          : session_value.prepare_datagram(std::span(payload_value.data(), payload_value.size()));
    if (plan.index() != 0) {
        throw std::length_error("HTTP Datagram payload exceeds its bound");
    }
    if (state_value.has_native_transport()) {
        return stream_.write_frame(udp ? std::string_view("\0", 1) : std::string_view{}, payload_value, true, std::move(operation));
    }
    return stream_.write_frame(std::get<0>(plan).prefix(), payload_value, false, std::move(operation));
}
task<void> http_datagram_stream::send_payload_owned(
    std::string&& payload_value, bool udp, operation_options operation) {
    if (!stream_.state_) {
        throw std::logic_error("HTTP Datagram stream is empty");
    }
    auto& state_value = *stream_.state_;
    state_value.require();
    if (state_value.send_ended_) {
        throw std::logic_error("HTTP Datagram sending direction is closed");
    }
    const auto context_bytes = udp ? std::size_t{1} : std::size_t{0};
    if (context_bytes > state_value.config_.max_capsule_length_ ||
        payload_value.size() > state_value.config_.max_capsule_length_ - context_bytes) {
        throw std::length_error("HTTP Datagram exceeds capsule bound");
    }
    http_datagram_session session_value;
    const auto plan = udp ? session_value.prepare_udp_datagram(std::span(payload_value.data(), payload_value.size()))
                          : session_value.prepare_datagram(std::span(payload_value.data(), payload_value.size()));
    if (plan.index() != 0) {
        throw std::length_error("HTTP Datagram payload exceeds its bound");
    }
    if (state_value.has_native_transport()) {
        return stream_.write_frame_owned(udp ? std::string_view("\0", 1) : std::string_view{},
            std::move(payload_value), true, std::move(operation));
    }
    return stream_.write_frame_owned(
        std::get<0>(plan).prefix(), std::move(payload_value), false, std::move(operation));
}
task<std::optional<http_datagram>> http_udp_tunnel::read(operation_options operation) & {
    auto* state_value = stream_.stream_.state_;
    if (!state_value) {
        throw std::logic_error("UDP tunnel is empty");
    }
    state_value->require();
    return ::ruvia::make_scoped_operation(state_value->read_scope_,
        http_datagram_stream::read_owned(detail::capsule_state_pin(*state_value), true, std::move(operation)));
}
task<void> http_udp_tunnel::send(std::span<const std::byte> payload_value, operation_options operation) & {
    return send(std::string_view(reinterpret_cast<const char*>(payload_value.data()), payload_value.size()),
        std::move(operation));
}
task<void> http_udp_tunnel::send(std::string_view payload_value, operation_options operation) & {
    return stream_.send_payload(payload_value, true, std::move(operation));
}
task<void> http_udp_tunnel::send_owned(
    std::string&& payload_value, operation_options operation) {
    return stream_.send_payload_owned(std::move(payload_value), true, std::move(operation));
}
}  // namespace ruvia

namespace ruvia::detail {

const std::unique_ptr<BIO_METHOD, decltype(&BIO_meth_free)> tls_tunnel_output::bio_method_ = [] {
    std::unique_ptr<BIO_METHOD, decltype(&BIO_meth_free)> method(
        BIO_meth_new(BIO_get_new_index() | BIO_TYPE_SOURCE_SINK, "Ruvia TLS tunnel output"), &BIO_meth_free);
    if (!method || BIO_meth_set_create(method.get(), &create_bio) != 1 ||
        BIO_meth_set_destroy(method.get(), &destroy_bio) != 1 ||
        BIO_meth_set_write_ex(method.get(), &write_bio) != 1 ||
        BIO_meth_set_ctrl(method.get(), &control_bio) != 1) {
        throw std::bad_alloc();
    }
    return method;
}();

tls_tunnel_output::tls_tunnel_output(SSL& ssl, asio::ip::tcp::socket& socket,
    const worker_ref& worker_value, connection_lifecycle* lifecycle)
    : ssl_(ssl),
      socket_(socket),
      worker_(worker_value),
      lifecycle_(lifecycle),
      available_(worker_value),
      flushed_(worker_value),
      tasks_(worker_value) {
    if (!worker_.is_current()) {
        std::terminate();
    }
    if (!SSL_is_init_finished(&ssl_)) {
        throw std::logic_error("TLS tunnel output requires an established TLS connection");
    }
    // Application writes are flushed in 4 KiB pieces. Space also covers TLS
    // control output generated from one buffered input record while a write waits.
    bytes_.resize(64 * 1024);
    auto bio = std::unique_ptr<BIO, decltype(&BIO_free)>(BIO_new(bio_method_.get()), &BIO_free);
    if (!bio) {
        throw std::bad_alloc();
    }
    previous_ = SSL_get_wbio(&ssl_);
    if (previous_ == nullptr || BIO_up_ref(previous_) != 1) {
        previous_ = nullptr;
        throw std::bad_alloc();
    }
    BIO_set_data(bio.get(), this);
    SSL_set0_wbio(&ssl_, bio.release());
}

tls_tunnel_output::~tls_tunnel_output() {
    if (!worker_.is_current() || tasks_.size() != 0) {
        std::terminate();
    }
    SSL_set0_wbio(&ssl_, previous_);
}

void tls_tunnel_output::start() {
    if (!worker_.is_current() || started_) {
        std::terminate();
    }
    tasks_.spawn(run_writer());
    started_ = true;
}

int tls_tunnel_output::create_bio(BIO* bio) noexcept {
    BIO_set_init(bio, 1);
    BIO_set_data(bio, nullptr);
    return 1;
}
int tls_tunnel_output::destroy_bio(BIO* bio) noexcept {
    BIO_set_data(bio, nullptr);
    BIO_set_init(bio, 0);
    return 1;
}
int tls_tunnel_output::write_bio(BIO* bio, const char* bytes_value, std::size_t size, std::size_t* written) noexcept {
    *written = 0;
    auto& output = *static_cast<tls_tunnel_output*>(BIO_get_data(bio));
    if (!output.worker_.is_current()) {
        std::terminate();
    }
    BIO_clear_retry_flags(bio);
    if (size == 0) {
        return 1;
    }
    const auto count = size;
    if (output.error_ || output.stopping_ || count > output.bytes_.size() - output.size_) {
        output.fail(std::make_error_code(std::errc::no_buffer_space));
        return 0;
    }
    const auto tail = (output.head_ + output.size_) % output.bytes_.size();
    const auto first = std::min(count, output.bytes_.size() - tail);
    std::memcpy(output.bytes_.data() + tail, bytes_value, first);
    std::memcpy(output.bytes_.data(), bytes_value + first, count - first);
    output.size_ += count;
    output.available_.notify();
    *written = size;
    return 1;
}
long tls_tunnel_output::control_bio(BIO*, int command, long, void*) noexcept {
    return command == BIO_CTRL_FLUSH ? 1 : 0;
}
void tls_tunnel_output::fail(std::error_code error) noexcept {
    if (!error_) {
        if (lifecycle_ != nullptr) {
            lifecycle_->note_error(error);
        }
        error_ = error;
    }
    close_socket(socket_);
    available_.notify();
    flushed_.notify();
}
void tls_tunnel_output::abort() noexcept {
    if (!worker_.is_current()) {
        std::terminate();
    }
    fail(std::make_error_code(std::errc::operation_canceled));
}
task<void> tls_tunnel_output::run_writer() {
    try {
        co_await run_writer_inner();
    } catch (...) {
        fail(std::make_error_code(std::errc::io_error));
        throw;
    }
}
task<void> tls_tunnel_output::run_writer_inner() {
    for (;;) {
        if (error_) {
            co_return;
        }
        if (size_ == 0) {
            flushed_.notify();
            if (stopping_) {
                co_return;
            }
            co_await available_.wait();
            continue;
        }
        const auto count = std::min(size_, bytes_.size() - head_);
        const auto result_value = co_await async_asio<std::size_t>([this, count](auto handler) {
            asio::async_write(socket_, asio::buffer(bytes_.data() + head_, count), std::move(handler));
        });
        if (lifecycle_ != nullptr) {
            lifecycle_->note_write(result_value.result());
        }
        if (result_value.error_code()) {
            fail(result_value.error_code());
            co_return;
        }
        head_ = (head_ + result_value.result()) % bytes_.size();
        size_ -= result_value.result();
        flushed_.notify();
    }
}
task<std::error_code> tls_tunnel_output::flush() {
    if (!worker_.is_current() || !started_) {
        std::terminate();
    }
    while (!error_ && size_ != 0) {
        co_await flushed_.wait();
    }
    co_return error_;
}
task<std::error_code> tls_tunnel_output::finish() {
    if (!worker_.is_current() || !started_) {
        std::terminate();
    }
    if (error_) {
        co_return error_;
    }
    if (!send_ended_) {
        ERR_clear_error();
        const auto result_value = SSL_shutdown(&ssl_);
        if (result_value < 0) {
            const auto code = ERR_get_error();
            fail(code == 0 ? std::make_error_code(std::errc::protocol_error)
                           : std::error_code(static_cast<int>(code), asio::error::get_ssl_category()));
            co_return error_;
        }
        send_ended_ = true;
    }
    co_return co_await flush();
}
task<void> tls_tunnel_output::join() {
    stopping_ = true;
    available_.notify();
    co_await tasks_.join();
}

}  // namespace ruvia::detail

namespace ruvia::detail {
namespace {

using security_callback = int (*)(const SSL*, const SSL_CTX*, int, int, int, void*, void*);

int enforce_security(const SSL*, const SSL_CTX*, int, int, int, void*, void*) noexcept;
void guard_native_handshake(const SSL*, int, int) noexcept;

struct tls_context_policy final {
    tls_context_policy(SSL_CTX& context, std::string_view host, std::uint64_t limit)
        : host_(host),
          record_limit_(limit),
          original_security_(SSL_CTX_get_security_callback(&context)),
          original_security_data_(SSL_CTX_get0_security_ex_data(&context)),
          original_info_(SSL_CTX_get_info_callback(&context)) {
        if (original_security_ == enforce_security) {
            const auto* previous = static_cast<const tls_context_policy*>(original_security_data_);
            original_security_ = previous->original_security_;
            original_security_data_ = previous->original_security_data_;
            if (original_info_ == guard_native_handshake) {
                original_info_ = previous->original_info_;
            }
        }
        if (!is_client_ip_address(host_) && host_.ends_with('.')) {
            host_.pop_back();
        }
    }
    std::string host_;
    std::uint64_t record_limit_;
    security_callback original_security_;
    void* original_security_data_;
    void (*original_info_)(const SSL*, int, int);
};

int enforce_security(const SSL* ssl, const SSL_CTX* context, int operation, int bits, int nid,
    void* other, void* opaque) noexcept {
    const auto& policy = *static_cast<const tls_context_policy*>(opaque);
    if (operation == SSL_SECOP_VERSION) {
        if (nid < TLS1_1_VERSION) {
            return 0;
        }
        if (nid == TLS1_1_VERSION) {
            return 1;
        }
    }
    if (operation == SSL_SECOP_COMPRESSION) {
        return 0;
    }
    if (operation == SSL_SECOP_CIPHER_SUPPORTED || operation == SSL_SECOP_CIPHER_SHARED ||
        operation == SSL_SECOP_CIPHER_CHECK) {
        const auto* cipher = static_cast<const SSL_CIPHER*>(other);
        switch (SSL_CIPHER_get_id(cipher)) {
            case TLS1_CK_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256:
            case TLS1_CK_ECDHE_RSA_WITH_AES_128_GCM_SHA256:
            case TLS1_CK_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384:
            case TLS1_CK_ECDHE_RSA_WITH_AES_256_GCM_SHA384:
            case TLS1_CK_ECDHE_ECDSA_WITH_CHACHA20_POLY1305:
            case TLS1_CK_ECDHE_RSA_WITH_CHACHA20_POLY1305:
                break;
            case TLS1_CK_ECDHE_ECDSA_WITH_AES_128_CBC_SHA:
            case TLS1_CK_ECDHE_RSA_WITH_AES_128_CBC_SHA:
            case TLS1_CK_ECDHE_ECDSA_WITH_AES_256_CBC_SHA:
            case TLS1_CK_ECDHE_RSA_WITH_AES_256_CBC_SHA:
                // SUPPORTED runs before the peer's version is known. Advertise
                // CBC for fallback, but never select or accept it with TLS 1.2.
                if (operation != SSL_SECOP_CIPHER_SUPPORTED && (!ssl || SSL_version(ssl) != TLS1_1_VERSION)) {
                    return 0;
                }
                break;
            default:
                if (std::string_view(SSL_CIPHER_get_version(cipher)) != "TLSv1.3") {
                    return 0;
                }
        }
    }
    if (ssl && SSL_version(ssl) == TLS1_1_VERSION &&
        (nid == NID_md5_sha1 || nid == NID_sha1) &&
        (operation == SSL_SECOP_SIGALG_SUPPORTED || operation == SSL_SECOP_SIGALG_SHARED ||
            operation == SSL_SECOP_SIGALG_CHECK || operation == SSL_SECOP_SIGALG_MASK)) {
        // These are handshake signature operations only. In particular,
        // SSL_SECOP_CA_MD, EE_KEY and CA_KEY are never exempted.
        return 1;
    }
    return policy.original_security_ &&
           policy.original_security_(ssl, context, operation, bits, nid, other, policy.original_security_data_);
}

int deny_security(const SSL*, const SSL_CTX*, int, int, int, void*, void*) noexcept {
    return 0;
}

struct tls_record_state final {
    std::uint64_t limit_;
    std::uint64_t records_{0};
    bool blocked_{false};
};

void free_context_policy(void*, void* value, CRYPTO_EX_DATA*, int, long, void*) noexcept {
    delete static_cast<tls_context_policy*>(value);
}

void free_record_state(void*, void* value, CRYPTO_EX_DATA*, int, long, void*) noexcept {
    delete static_cast<tls_record_state*>(value);
}

int copy_record_state(CRYPTO_EX_DATA*, const CRYPTO_EX_DATA*, void** value, int, long, void*) noexcept {
    try {
        if (*value) {
            *value = new tls_record_state(*static_cast<tls_record_state*>(*value));
        }
        return 1;
    } catch (...) {
        return 0;
    }
}

int context_index = -1;
int record_index = -1;
int bio_index = -1;

void guard_native_handshake(const SSL* ssl, int where, int result) noexcept {
    if ((where & SSL_CB_HANDSHAKE_START) != 0) {
        // Unlike SSL_new's ex_data notification, this runs only after the
        // connection is initialized and before any handshake is processed.
        SSL_set_options(const_cast<SSL*>(ssl), SSL_OP_NO_RENEGOTIATION | SSL_OP_NO_COMPRESSION);
    }
    const auto* policy = static_cast<const tls_context_policy*>(
        SSL_CTX_get_ex_data(SSL_get_SSL_CTX(ssl), context_index));
    if (policy && policy->original_info_) {
        policy->original_info_(ssl, where, result);
    }
}

long deny_output(BIO* bio, int operation, const char*, std::size_t, int, long, int result,
    std::size_t* processed) noexcept {
    if (operation == BIO_CB_WRITE) {
        ERR_raise_data(ERR_LIB_SSL, ERR_R_OPERATION_FAIL, "TLS outgoing record limit or policy allocation failure");
        BIO_clear_retry_flags(bio);
        if (processed) {
            *processed = 0;
        }
        return 0;
    }
    return result;
}

long guard_output(BIO* bio, int operation, const char* bytes, std::size_t size, int arg,
    long larg, int result, std::size_t* processed) noexcept {
    if (operation == BIO_CB_WRITE) {
        const auto* state = static_cast<const tls_record_state*>(BIO_get_ex_data(bio, bio_index));
        if (!state || state->blocked_) {
            return deny_output(bio, operation, bytes, size, arg, larg, result, processed);
        }
    }
    return result;
}

void block_messages(int writing, int, int, const void*, std::size_t, SSL* ssl, void*) noexcept {
    if (writing) {
        if (auto* bio = SSL_get_wbio(ssl)) {
            BIO_set_callback_ex(bio, deny_output);
        }
    }
}

}  // namespace

void count_tls_records(int writing, int, int content_type, const void*, std::size_t, SSL* ssl, void*) noexcept {
    if (!writing) {
        return;
    }
    auto* bio = SSL_get_wbio(ssl);
    if (!bio) {
        return;
    }
    try {
        auto* state = static_cast<tls_record_state*>(SSL_get_ex_data(ssl, record_index));
        if (!state) {
            const auto* policy = static_cast<const tls_context_policy*>(SSL_CTX_get_ex_data(SSL_get_SSL_CTX(ssl), context_index));
            if (!policy) {
                BIO_set_callback_ex(bio, deny_output);
                return;
            }
            auto owner = std::make_unique<tls_record_state>(tls_record_state{policy->record_limit_});
            if (SSL_set_ex_data(ssl, record_index, owner.get()) != 1) {
                throw std::bad_alloc();
            }
            state = owner.release();
        }
        if (content_type == SSL3_RT_HEADER) {
            if (state->records_ == state->limit_) {
                state->blocked_ = true;
            } else {
                ++state->records_;
            }
        }
        // CONNECT 会替换写 BIO；所有密文生产者仍绑定同一连接级计数。
        if (BIO_get_ex_data(bio, bio_index) != state && BIO_set_ex_data(bio, bio_index, state) != 1) {
            throw std::bad_alloc();
        }
        BIO_set_callback_ex(bio, guard_output);
    } catch (...) {
        ERR_raise(ERR_LIB_SSL, ERR_R_MALLOC_FAILURE);
        BIO_set_callback_ex(bio, deny_output);
    }
}

namespace {

int accept_verified_chain(int valid, X509_STORE_CTX*) noexcept {
    return valid;
}

int deny_certificate(X509_STORE_CTX*, void*) noexcept {
    return 0;
}

int verify_native_certificate(X509_STORE_CTX* store, void* opaque) noexcept {
    const auto& policy = *static_cast<const tls_context_policy*>(opaque);
    // MariaDB 的默认回调会延迟并容忍部分错误；这里在握手内完成严格校验。
    X509_STORE_CTX_set_verify_cb(store, accept_verified_chain);
    if (X509_verify_cert(store) != 1) {
        return 0;
    }
    auto* leaf = X509_STORE_CTX_get0_cert(store);
    if (!leaf) {
        X509_STORE_CTX_set_error(store, X509_V_ERR_APPLICATION_VERIFICATION);
        return 0;
    }
    const bool ip = is_client_ip_address(policy.host_);
    const auto matched = ip ? X509_check_ip_asc(leaf, policy.host_.c_str(), 0)
                            : X509_check_host(leaf, policy.host_.c_str(), policy.host_.size(),
                                  X509_CHECK_FLAG_NEVER_CHECK_SUBJECT | X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS, nullptr);
    if (matched == 1) {
        return 1;
    }
    X509_STORE_CTX_set_error(store, matched == 0
                                        ? (ip ? X509_V_ERR_IP_ADDRESS_MISMATCH : X509_V_ERR_HOSTNAME_MISMATCH)
                                        : X509_V_ERR_APPLICATION_VERIFICATION);
    return 0;
}

void install_context_callbacks(SSL_CTX& context, tls_context_policy* policy) noexcept {
    SSL_CTX_set0_security_ex_data(&context, policy);
    SSL_CTX_set_security_callback(&context, enforce_security);
    SSL_CTX_set_msg_callback(&context, count_tls_records);
    SSL_CTX_set_msg_callback_arg(&context, nullptr);
    if (!policy->host_.empty()) {
        SSL_CTX_set_cert_verify_callback(&context, verify_native_certificate, policy);
        SSL_CTX_set_info_callback(&context, guard_native_handshake);
    }
}

}  // namespace

thread_local native_tls_context_scope* native_tls_context_scope::current_ = nullptr;

// Registers, once per process, the ex_data slots that carry the policy on
// every context, connection and BIO.
void register_tls_policy_indexes() {
    static const bool registered = [] {
        context_index = SSL_CTX_get_ex_new_index(
            0, nullptr, native_tls_context_scope::new_context, nullptr, free_context_policy);
        record_index = SSL_get_ex_new_index(0, nullptr, nullptr, copy_record_state, free_record_state);
        bio_index = BIO_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr);
        if (context_index < 0 || record_index < 0 || bio_index < 0) {
            throw std::runtime_error("failed to register TLS policy storage");
        }
        return true;
    }();
    (void)registered;
}

native_tls_context_scope::native_tls_context_scope(std::string_view host, std::uint64_t record_limit)
    : host_(host),
      record_limit_(record_limit),
      previous_(current_) {
    // 构造通知只安装回调和 ex_data；不查询未初始化的 context 参数或 SSL 状态。
    register_tls_policy_indexes();
    current_ = host_.empty() ? nullptr : this;
}

native_tls_context_scope::~native_tls_context_scope() {
    current_ = previous_;
}

void native_tls_context_scope::rethrow_failure() const {
    if (failure_) {
        std::rethrow_exception(failure_);
    }
}

void native_tls_context_scope::new_context(void* parent, void*, CRYPTO_EX_DATA* storage, int index,
    long, void*) noexcept {
    if (!current_) {
        return;
    }
    auto* context = static_cast<SSL_CTX*>(parent);
    try {
        auto owner = std::make_unique<tls_context_policy>(*context, current_->host_, current_->record_limit_);
        if (CRYPTO_set_ex_data(storage, index, owner.get()) != 1) {
            throw std::bad_alloc();
        }
        install_context_callbacks(*context, owner.release());
    } catch (...) {
        current_->failure_ = std::current_exception();
        SSL_CTX_set_security_callback(context, deny_security);
        SSL_CTX_set_cert_verify_callback(context, deny_certificate, nullptr);
        SSL_CTX_set_msg_callback(context, block_messages);
    }
}

void validate_tls_record_limit(std::uint64_t limit) {
    if (limit == 0 || limit > (std::uint64_t{1} << 23)) {
        throw std::invalid_argument("TLS outgoing record limit must be in 1..2^23");
    }
}

void apply_tls_protocol_policy(SSL_CTX& context, std::uint64_t record_limit) {
    validate_tls_record_limit(record_limit);
    register_tls_policy_indexes();
    auto owner = std::make_unique<tls_context_policy>(context, std::string_view{}, record_limit);
    auto* previous = static_cast<tls_context_policy*>(SSL_CTX_get_ex_data(&context, context_index));
    if (SSL_CTX_set_ex_data(&context, context_index, owner.get()) != 1) {
        throw std::bad_alloc();
    }
    auto* policy = owner.release();
    delete previous;
    install_context_callbacks(context, policy);
    if (SSL_CTX_set_min_proto_version(&context, TLS1_1_VERSION) != 1 || SSL_CTX_set_max_proto_version(&context, 0) != 1) {
        throw std::runtime_error("failed to configure TLS versions");
    }
    SSL_CTX_set_options(&context, SSL_OP_NO_COMPRESSION | SSL_OP_NO_RENEGOTIATION);
    if (SSL_CTX_set_cipher_list(&context, tls_cipher_suites) != 1) {
        throw std::runtime_error("failed to configure TLS cipher suites");
    }
}

namespace {

[[nodiscard]] std::string_view origin_key(const http_target_origin& origin) noexcept {
    if (origin.host_kind() == http_target_host_kind::dns_name) {
        return origin.host();
    }
    const auto address = origin.address();
    return {reinterpret_cast<const char*>(address.data()), address.size()};
}

}  // namespace

bool tls_served_certificate::covers(const http_target_origin& origin) noexcept {
    if (origin.action() != http_target_origin_action::verify_certificate) {
        return false;
    }
    const auto key = origin_key(origin);
    if (key.empty()) {
        return false;
    }
    const bool dns_name = origin.host_kind() == http_target_host_kind::dns_name;
    for (std::size_t i = 0; i < cached_; ++i) {
        const auto& entry = cache_[i];
        if (entry.kind_ != origin.host_kind() || entry.key_size_ != key.size()) {
            continue;
        }
        const std::string_view cached_key(key_storage_.data() + entry.key_offset_, entry.key_size_);
        if (dns_name ? http_ascii_equals_ignore_case(cached_key, key) : cached_key == key) {
            return entry.covered_;
        }
    }
    const bool covered = check(origin);
    if (key.size() <= cached_key_capacity) {
        if (cached_ == cache_size || key.size() > key_storage_capacity - key_used_) {
            cached_ = 0;
            key_used_ = 0;
        }
        auto& entry = cache_[cached_++];
        entry.key_offset_ = static_cast<std::uint8_t>(key_used_);
        entry.key_size_ = static_cast<std::uint8_t>(key.size());
        entry.kind_ = origin.host_kind();
        entry.covered_ = covered;
        std::copy_n(key.data(), key.size(), key_storage_.data() + key_used_);
        key_used_ = static_cast<std::uint16_t>(key_used_ + key.size());
    }
    return covered;
}

bool tls_served_certificate::check(const http_target_origin& origin) const noexcept {
    if (certificate_ == nullptr) {
        return false;
    }
    if (origin.host_kind() == http_target_host_kind::dns_name) {
        const auto host = origin.host();
        return X509_check_host(certificate_, host.data(), host.size(),
                   X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS | X509_CHECK_FLAG_NEVER_CHECK_SUBJECT, nullptr) == 1;
    }
    const auto address = origin.address();
    return X509_check_ip(certificate_, address.data(), address.size(), 0) == 1;
}

}  // namespace ruvia::detail

namespace ruvia::detail {
namespace {

struct ssl_context_deleter final {
    void operator()(SSL_CTX* context_value) const noexcept {
        SSL_CTX_free(context_value);
    }
};

struct x509_store_deleter final {
    void operator()(X509_STORE* store) const noexcept {
        X509_STORE_free(store);
    }
};

struct bio_deleter final {
    void operator()(BIO* bio) const noexcept {
        BIO_free(bio);
    }
};

struct x509_deleter final {
    void operator()(X509* certificate) const noexcept {
        X509_free(certificate);
    }
};

struct private_key_deleter final {
    void operator()(EVP_PKEY* private_key) const noexcept {
        EVP_PKEY_free(private_key);
    }
};

// A read-only BIO over caller-owned PEM text, which it never copies.
[[nodiscard]] std::unique_ptr<BIO, bio_deleter> open_pem_text(std::string_view text) {
    if (!std::in_range<int>(text.size())) {
        throw std::invalid_argument("TLS PEM text is too large");
    }
    ::ERR_clear_error();
    std::unique_ptr<BIO, bio_deleter> input(BIO_new_mem_buf(text.data(), static_cast<int>(text.size())));
    if (input == nullptr) {
        throw_tls_credential_error("BIO_new_mem_buf");
    }
    return input;
}

// SSL_CTX_use_certificate_chain_file() over memory: the first certificate is
// the leaf, every later one joins its chain, and only the end of the text
// ends the chain.
void use_certificate_chain_pem(SSL_CTX& context, std::string_view text) {
    const auto input = open_pem_text(text);
    auto* const password_callback = SSL_CTX_get_default_passwd_cb(&context);
    auto* const password_userdata = SSL_CTX_get_default_passwd_cb_userdata(&context);
    const std::unique_ptr<X509, x509_deleter> leaf(
        PEM_read_bio_X509_AUX(input.get(), nullptr, password_callback, password_userdata));
    if (leaf == nullptr || SSL_CTX_use_certificate(&context, leaf.get()) != 1) {
        throw_tls_credential_error("use_certificate_chain_pem");
    }
    for (;;) {
        std::unique_ptr<X509, x509_deleter> issuer(
            PEM_read_bio_X509(input.get(), nullptr, password_callback, password_userdata));
        if (issuer == nullptr) {
            break;
        }
        if (SSL_CTX_add0_chain_cert(&context, issuer.get()) != 1) {
            throw_tls_credential_error("use_certificate_chain_pem");
        }
        // The context owns it now.
        (void)issuer.release();
    }
    const auto error = ::ERR_peek_last_error();
    if (ERR_GET_LIB(error) != ERR_LIB_PEM || ERR_GET_REASON(error) != PEM_R_NO_START_LINE) {
        throw_tls_credential_error("use_certificate_chain_pem");
    }
    ::ERR_clear_error();
}

void use_private_key_pem(SSL_CTX& context, std::string_view text) {
    const auto input = open_pem_text(text);
    const std::unique_ptr<EVP_PKEY, private_key_deleter> private_key(PEM_read_bio_PrivateKey(input.get(), nullptr,
        SSL_CTX_get_default_passwd_cb(&context), SSL_CTX_get_default_passwd_cb_userdata(&context)));
    if (private_key == nullptr || SSL_CTX_use_PrivateKey(&context, private_key.get()) != 1) {
        throw_tls_credential_error("use_private_key_pem");
    }
}

// Borrow the configured password only during synchronous credential loading.
// An absent password uses a caller provider, or an empty noninteractive default.
class tls_password_scope final {
public:
    tls_password_scope(SSL_CTX& context_value, std::string_view password) noexcept
        : context_(&context_value),
          password_(password),
          previous_callback_(SSL_CTX_get_default_passwd_cb(&context_value)),
          previous_userdata_(SSL_CTX_get_default_passwd_cb_userdata(&context_value)) {
        const bool default_source = previous_userdata_ == nullptr &&
                                    (previous_callback_ == nullptr || previous_callback_ == PEM_def_callback);
        if (!password.empty() || default_source) {
            SSL_CTX_set_default_passwd_cb(context_, &copy_password);
            SSL_CTX_set_default_passwd_cb_userdata(context_, this);
        }
    }

    ~tls_password_scope() {
        // The caller may own its userdata: Asio deletes its callback object.
        SSL_CTX_set_default_passwd_cb(context_, previous_callback_);
        SSL_CTX_set_default_passwd_cb_userdata(context_, previous_userdata_);
    }

    tls_password_scope(const tls_password_scope&) = delete;
    tls_password_scope& operator=(const tls_password_scope&) = delete;
    tls_password_scope(tls_password_scope&&) = delete;
    tls_password_scope& operator=(tls_password_scope&&) = delete;

private:
    static int copy_password(char* buffer, int size, int, void* userdata) noexcept {
        if (buffer == nullptr || size <= 0 || userdata == nullptr) {
            return -1;
        }
        const auto password = static_cast<const tls_password_scope*>(userdata)->password_;
        if (password.size() > static_cast<std::size_t>(size)) {
            return -1;
        }
        if (!password.empty()) {
            std::memcpy(buffer, password.data(), password.size());
        }
        return static_cast<int>(password.size());
    }

    SSL_CTX* context_;
    std::string_view password_;
    pem_password_cb* previous_callback_;
    void* previous_userdata_;
};

// OpenSSL's PEM loaders resolve the full chain and the (possibly encrypted)
// key into a scratch context, which keeps that one authoritative parser.
[[nodiscard]] std::unique_ptr<SSL_CTX, ssl_context_deleter> make_scratch_context() {
    ::ERR_clear_error();
    std::unique_ptr<SSL_CTX, ssl_context_deleter> scratch(SSL_CTX_new(TLS_method()));
    if (scratch == nullptr) {
        throw_tls_credential_error("SSL_CTX_new");
    }
    return scratch;
}

}  // namespace

void throw_tls_credential_error(const char* operation) {
    const auto code = ::ERR_get_error();
    const auto error = ERR_SYSTEM_ERROR(code)
                           ? std::error_code(ERR_GET_REASON(code), asio::error::get_system_category())
                           : std::error_code(static_cast<int>(code), asio::error::get_ssl_category());
    throw asio::system_error(error, operation);
}

tls_certificate tls_certificate::load(const char* certificate_chain_file, const char* private_key_file,
    std::string_view private_key_password) {
    const auto scratch = make_scratch_context();
    {
        const tls_password_scope password_scope(*scratch, private_key_password);
        ::ERR_clear_error();
        if (::SSL_CTX_use_certificate_chain_file(scratch.get(), certificate_chain_file) != 1) {
            throw_tls_credential_error("use_certificate_chain_file");
        }
        ::ERR_clear_error();
        if (::SSL_CTX_use_PrivateKey_file(scratch.get(), private_key_file, SSL_FILETYPE_PEM) != 1) {
            throw_tls_credential_error("use_private_key_file");
        }
    }
    return adopt_loaded(*scratch);
}

tls_certificate tls_certificate::load_pem(std::string_view certificate_chain, std::string_view private_key,
    std::string_view private_key_password) {
    const auto scratch = make_scratch_context();
    {
        const tls_password_scope password_scope(*scratch, private_key_password);
        use_certificate_chain_pem(*scratch, certificate_chain);
        use_private_key_pem(*scratch, private_key);
    }
    return adopt_loaded(*scratch);
}

tls_certificate tls_certificate::adopt_loaded(SSL_CTX& scratch) {
    ::ERR_clear_error();
    if (::SSL_CTX_check_private_key(&scratch) != 1) {
        throw_tls_credential_error("check_private_key");
    }
    STACK_OF(X509)* chain = nullptr;
    if (SSL_CTX_get0_chain_certs(&scratch, &chain) != 1) {
        throw_tls_credential_error("get0_chain_certs");
    }
    return tls_certificate(SSL_CTX_get0_certificate(&scratch), chain, SSL_CTX_get0_privatekey(&scratch));
}

tls_certificate::tls_certificate(X509* certificate, STACK_OF(X509) * chain, EVP_PKEY* private_key) {
    if (certificate == nullptr || private_key == nullptr) {
        throw std::invalid_argument("TLS certificate and private key must not be null");
    }
    if (chain != nullptr) {
        chain_ = X509_chain_up_ref(chain);
        if (chain_ == nullptr) {
            throw std::bad_alloc();
        }
    }
    if (X509_up_ref(certificate) != 1) {
        OSSL_STACK_OF_X509_free(chain_);
        chain_ = nullptr;
        throw std::bad_alloc();
    }
    certificate_ = certificate;
    if (EVP_PKEY_up_ref(private_key) != 1) {
        X509_free(certificate_);
        certificate_ = nullptr;
        OSSL_STACK_OF_X509_free(chain_);
        chain_ = nullptr;
        throw std::bad_alloc();
    }
    private_key_ = private_key;
}

tls_certificate::tls_certificate(const tls_certificate& other)
    : tls_certificate(other.certificate_, other.chain_, other.private_key_) {}

tls_certificate::tls_certificate(tls_certificate&& other) noexcept
    : certificate_(std::exchange(other.certificate_, nullptr)),
      chain_(std::exchange(other.chain_, nullptr)),
      private_key_(std::exchange(other.private_key_, nullptr)) {}

tls_certificate& tls_certificate::operator=(tls_certificate&& other) noexcept {
    if (this != &other) {
        release();
        certificate_ = std::exchange(other.certificate_, nullptr);
        chain_ = std::exchange(other.chain_, nullptr);
        private_key_ = std::exchange(other.private_key_, nullptr);
    }
    return *this;
}

tls_certificate::~tls_certificate() {
    release();
}

void tls_certificate::release() noexcept {
    X509_free(certificate_);
    OSSL_STACK_OF_X509_free(chain_);
    EVP_PKEY_free(private_key_);
    certificate_ = nullptr;
    chain_ = nullptr;
    private_key_ = nullptr;
}

void tls_certificate::install(SSL_CTX& context) const {
    ::ERR_clear_error();
    if (::SSL_CTX_use_certificate(&context, certificate_) != 1) {
        throw_tls_credential_error("use_certificate");
    }
    ::ERR_clear_error();
    if (::SSL_CTX_use_PrivateKey(&context, private_key_) != 1) {
        throw_tls_credential_error("use_private_key");
    }
    ::ERR_clear_error();
    if (chain_ != nullptr && SSL_CTX_set1_chain(&context, chain_) != 1) {
        throw_tls_credential_error("set1_chain");
    }
    ::ERR_clear_error();
    if (::SSL_CTX_check_private_key(&context) != 1) {
        throw_tls_credential_error("check_private_key");
    }
}

bool tls_certificate::present(SSL& connection) const noexcept {
    // Override the whole slot: an absent chain_ clears any earlier chain.
    return ::SSL_use_cert_and_key(&connection, certificate_, private_key_, chain_, 1) == 1;
}

tls_trust_store tls_trust_store::load_file(const char* file) {
    ::ERR_clear_error();
    std::unique_ptr<X509_STORE, x509_store_deleter> store(X509_STORE_new());
    if (store == nullptr) {
        throw_tls_credential_error("X509_STORE_new");
    }
    if (X509_STORE_load_file(store.get(), file) != 1) {
        throw_tls_credential_error("load_verify_file");
    }
    return tls_trust_store(store.release());
}

tls_trust_store tls_trust_store::load_default() {
    ::ERR_clear_error();
    std::unique_ptr<X509_STORE, x509_store_deleter> store(X509_STORE_new());
    if (store == nullptr) {
        throw_tls_credential_error("X509_STORE_new");
    }
    if (X509_STORE_set_default_paths(store.get()) != 1) {
        throw_tls_credential_error("set_default_verify_paths");
    }
    return tls_trust_store(store.release());
}

tls_trust_store::tls_trust_store(const tls_trust_store& other)
    : store_(other.store_) {
    if (store_ != nullptr && X509_STORE_up_ref(store_) != 1) {
        store_ = nullptr;
        throw std::bad_alloc();
    }
}

tls_trust_store::tls_trust_store(tls_trust_store&& other) noexcept
    : store_(std::exchange(other.store_, nullptr)) {}

tls_trust_store& tls_trust_store::operator=(const tls_trust_store& other) {
    if (this != &other) {
        *this = tls_trust_store(other);
    }
    return *this;
}

tls_trust_store& tls_trust_store::operator=(tls_trust_store&& other) noexcept {
    if (this != &other) {
        X509_STORE_free(store_);
        store_ = std::exchange(other.store_, nullptr);
    }
    return *this;
}

tls_trust_store::~tls_trust_store() {
    X509_STORE_free(store_);
}

void tls_trust_store::install(SSL_CTX& context) const noexcept {
    SSL_CTX_set1_cert_store(&context, store_);
}

}  // namespace ruvia::detail

namespace ruvia {
namespace {

struct cleanse_identity final {
    tls_identity_config& identity_;
    ~cleanse_identity() {
        for (auto* secret : {&identity_.certificate_chain_pem_, &identity_.private_key_pem_,
                 &identity_.private_key_password_}) {
            if (!secret->empty()) {
                OPENSSL_cleanse(secret->data(), secret->size());
                secret->clear();
            }
        }
    }
};

// Never move a secret-bearing config between coroutine/lambda frames: an SSO
// move may leave bytes in its source. One stable, process-owned payload crosses
// the pool boundary, and every incoming/partially populated copy is cleansed.
struct owned_identity final {
    tls_identity_config value_;
    ~owned_identity() {
        const cleanse_identity cleanse{value_};
    }
};

using owned_identity_pointer = std::unique_ptr<owned_identity>;

void require_accepting_caller(const worker_ref& caller) {
    if (!caller.accepting()) {
        throw std::runtime_error("TLS certificate mutation caller is stopping");
    }
}

}  // namespace

struct tls_certificate_store::state final {
    detail::runtime_control_gate writer_;
    std::atomic<std::shared_ptr<const detail::tls_certificate_snapshot>> snapshot_{
        std::make_shared<const detail::tls_certificate_snapshot>()};

    [[nodiscard]] static task<void> insert_or_assign(
        std::shared_ptr<state> owner, worker_ref caller, tls_certificate_id id,
        owned_identity_pointer identity) {
        const auto lease = co_await owner->writer_.acquire(caller);
        require_accepting_caller(caller);
        auto prepared = co_await run_blocking(caller,
            [current = owner->snapshot_.load(std::memory_order_acquire), id,
                identity = std::move(identity)]() mutable {
                auto certificate = std::make_shared<const detail::tls_certificate>(
                    detail::load_server_tls_certificate(identity->value_));
                identity.reset();
                auto next = std::make_shared<detail::tls_certificate_snapshot>(*current);
                next->insert_or_assign(id, std::move(certificate));
                return next;
            });
        auto next = std::move(prepared).take_value();
        require_accepting_caller(caller);
        owner->snapshot_.store(std::move(next), std::memory_order_release);
    }

    [[nodiscard]] static task<bool> erase(
        std::shared_ptr<state> owner, worker_ref caller, tls_certificate_id id) {
        const auto lease = co_await owner->writer_.acquire(caller);
        require_accepting_caller(caller);
        auto current = owner->snapshot_.load(std::memory_order_acquire);
        if (!current->contains(id)) {
            co_return false;
        }
        // Copying an arbitrarily large immutable map is real preparation, not
        // a synchronous erase disguised by posting a closure to the caller.
        auto prepared = co_await run_blocking(caller,
            [current = std::move(current), id] {
                auto next = std::make_shared<detail::tls_certificate_snapshot>(*current);
                next->erase(id);
                return next;
            });
        auto next = std::move(prepared).take_value();
        require_accepting_caller(caller);
        owner->snapshot_.store(std::move(next), std::memory_order_release);
        co_return true;
    }
};

tls_certificate_store::tls_certificate_store()
    : state_(std::make_shared<state>()) {}
tls_certificate_store::~tls_certificate_store() = default;

task<void> tls_certificate_store::insert_or_assign(
    worker_ref caller, tls_certificate_id id, tls_identity_config identity) {
    const cleanse_identity cleanse{identity};
    auto owner = state_;
    if (id == 0) {
        throw std::invalid_argument("TLS certificate ID zero is reserved");
    }
    auto owned = std::make_unique<owned_identity>();
    owned->value_.certificate_chain_file_ = std::move(identity.certificate_chain_file_);
    owned->value_.private_key_file_ = std::move(identity.private_key_file_);
    owned->value_.certificate_chain_pem_.assign(identity.certificate_chain_pem_);
    owned->value_.private_key_pem_.assign(identity.private_key_pem_);
    owned->value_.private_key_password_.assign(identity.private_key_password_);
    return state::insert_or_assign(std::move(owner), std::move(caller), id, std::move(owned));
}

task<bool> tls_certificate_store::erase(worker_ref caller, tls_certificate_id id) {
    return state::erase(state_, std::move(caller), id);
}

bool tls_certificate_store::contains(tls_certificate_id id) const noexcept {
    return state_->snapshot_.load(std::memory_order_acquire)->contains(id);
}

std::size_t tls_certificate_store::size() const noexcept {
    return state_->snapshot_.load(std::memory_order_acquire)->size();
}

bool tls_certificate_selection::offers_signature_algorithm(std::uint16_t algorithm) const noexcept {
    return std::ranges::find(signature_algorithms_, algorithm) != signature_algorithms_.end();
}

std::shared_ptr<const detail::tls_certificate_snapshot> detail::tls_certificate_store_access::snapshot(
    const tls_certificate_store& store) noexcept {
    return store.state_->snapshot_.load(std::memory_order_acquire);
}

}  // namespace ruvia

namespace ruvia::detail {
namespace {

[[nodiscard]] std::optional<tls_session_ticket_key> generate_ticket_key() noexcept {
    tls_session_ticket_key key;
    if (!fill_random_bytes(key.name_) || !fill_random_bytes(key.hmac_key_) || !fill_random_bytes(key.aes_key_)) {
        return std::nullopt;
    }
    return key;
}

// The ex_data slot that points every TLS context of a listener at its
// worker's key set. Allocated by the first install(), at startup.
[[nodiscard]] int ticket_key_set_index() noexcept {
    static const int index = SSL_CTX_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr);
    return index;
}

[[nodiscard]] bool set_ticket_mac_key(EVP_MAC_CTX& mac, const tls_session_ticket_key& key) noexcept {
    static char digest[] = "SHA256";
    const OSSL_PARAM parameters[] = {
        OSSL_PARAM_construct_octet_string(
            OSSL_MAC_PARAM_KEY, const_cast<unsigned char*>(key.hmac_key_.data()), key.hmac_key_.size()),
        OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_DIGEST, digest, 0),
        OSSL_PARAM_construct_end(),
    };
    return EVP_MAC_CTX_set_params(&mac, parameters) == 1;
}

[[nodiscard]] bool names_key(const std::optional<tls_session_ticket_key>& key, const unsigned char* name) noexcept {
    return key.has_value() && CRYPTO_memcmp(key->name_.data(), name, key->name_.size()) == 0;
}

}  // namespace

tls_session_ticket_key::~tls_session_ticket_key() {
    OPENSSL_cleanse(name_.data(), name_.size());
    OPENSSL_cleanse(hmac_key_.data(), hmac_key_.size());
    OPENSSL_cleanse(aes_key_.data(), aes_key_.size());
}

tls_session_ticket_key_ring::tls_session_ticket_key_ring(std::chrono::steady_clock::time_point origin)
    : origin_(origin),
      current_(generate_ticket_key()) {
    if (!current_.has_value()) {
        throw std::runtime_error("failed to generate TLS session ticket keys");
    }
}

std::uint64_t tls_session_ticket_key_ring::epoch_at(std::chrono::steady_clock::time_point now) const noexcept {
    if (now <= origin_) {
        return 0;
    }
    return static_cast<std::uint64_t>((now - origin_) / tls_session_lifetime);
}

std::chrono::steady_clock::time_point tls_session_ticket_key_ring::next_rotation(
    std::chrono::steady_clock::time_point now) const noexcept {
    return origin_ + (epoch_at(now) + 1) * std::chrono::duration_cast<std::chrono::steady_clock::duration>(tls_session_lifetime);
}

bool tls_session_ticket_key_ring::copy_to(std::chrono::steady_clock::time_point now, tls_session_ticket_key_set& target) {
    std::optional<tls_session_ticket_key> retired_previous;
    std::optional<tls_session_ticket_key> retired_current;
    std::optional<tls_session_ticket_key> retired_target_current;
    std::optional<tls_session_ticket_key> retired_target_previous;
    bool has_current = false;
    {
        const std::lock_guard lock(mutex_);
        const auto epoch = epoch_at(now);
        if (epoch > epoch_) {
            // The previous key's validity ended with the old epoch; the current
            // key stays valid for decryption through exactly one more epoch.
            retired_previous = std::move(previous_);
            previous_.reset();
            if (epoch == epoch_ + 1) {
                previous_ = current_;
            }
            retired_current = std::move(current_);
            current_.reset();
            epoch_ = epoch;
        }
        if (!current_.has_value()) {
            current_ = generate_ticket_key();
        }
        retired_target_current = std::exchange(target.current_, current_);
        retired_target_previous = std::exchange(target.previous_, previous_);
        has_current = current_.has_value();
    }
    return has_current;
}

tls_session_ticket_key_set::tls_session_ticket_key_set()
    : aes_256_cbc_(EVP_CIPHER_fetch(nullptr, "AES-256-CBC", nullptr)) {
    if (aes_256_cbc_ == nullptr) {
        throw std::runtime_error("failed to fetch the TLS session ticket cipher");
    }
}

tls_session_ticket_key_set::~tls_session_ticket_key_set() {
    EVP_CIPHER_free(aes_256_cbc_);
}

void tls_session_ticket_key_set::install(SSL_CTX& context) {
    const int index = ticket_key_set_index();
    if (index < 0 || SSL_CTX_set_ex_data(&context, index, this) != 1 ||
        SSL_CTX_set_tlsext_ticket_key_evp_cb(&context, &tls_session_ticket_key_set::protect_ticket) != 1) {
        throw std::runtime_error("failed to install TLS session ticket keys");
    }
    (void)SSL_CTX_set_timeout(&context, static_cast<long>(tls_session_lifetime.count()));
}

// SSL_CTX_set_tlsext_ticket_key_evp_cb(3): returns 1 when the key is set up,
// 2 when a ticket decrypted with the previous key should be renewed, 0 when no
// ticket is issued or the key name is unknown (full handshake), -1 on error.
int tls_session_ticket_key_set::protect_ticket(SSL* ssl, unsigned char* key_name, unsigned char* iv,
    EVP_CIPHER_CTX* cipher, EVP_MAC_CTX* mac, int encrypt) noexcept {
    // OpenSSL calls the callback of the connection's initial context, but an
    // SNI switch may already have replaced the context; every context of the
    // listener points at the same set.
    const auto* keys = static_cast<const tls_session_ticket_key_set*>(
        SSL_CTX_get_ex_data(SSL_get_SSL_CTX(ssl), ticket_key_set_index()));
    if (keys == nullptr || cipher == nullptr || mac == nullptr) {
        return -1;
    }
    if (encrypt != 0) {
        if (!keys->current_.has_value()) {
            return 0;
        }
        const auto& key = *keys->current_;
        std::ranges::copy(key.name_, key_name);
        const int iv_size = EVP_CIPHER_get_iv_length(keys->aes_256_cbc_);
        if (iv_size <= 0 || iv_size > EVP_MAX_IV_LENGTH || RAND_bytes(iv, iv_size) != 1 ||
            EVP_EncryptInit_ex2(cipher, keys->aes_256_cbc_, key.aes_key_.data(), iv, nullptr) != 1 ||
            !set_ticket_mac_key(*mac, key)) {
            return -1;
        }
        return 1;
    }
    const bool current = names_key(keys->current_, key_name);
    if (!current && !names_key(keys->previous_, key_name)) {
        return 0;
    }
    const auto& key = current ? *keys->current_ : *keys->previous_;
    if (!set_ticket_mac_key(*mac, key) ||
        EVP_DecryptInit_ex2(cipher, keys->aes_256_cbc_, key.aes_key_.data(), iv, nullptr) != 1) {
        return -1;
    }
    return current ? 1 : 2;
}

}  // namespace ruvia::detail
