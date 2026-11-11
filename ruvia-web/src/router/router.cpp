#include <algorithm>
#include <bit>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/http/http_request_target.h"
#include "ruvia/http/url_encoding.h"
#include "ruvia/web/detail/app/app_configuration.h"
#include "ruvia/web/detail/router/prefix_fallback.h"

#include "router/path_segments.h"
#include "router/router_impl.h"

namespace ruvia {

using namespace detail;

namespace {

// app_configuration already normalized, null-checked and de-duplicated the prefixes.
template <typename stored_type, typename registration_type>
void replace_prefix_handlers(std::pmr::vector<stored_type>& stored, std::span<const registration_type> handlers) {
    auto* const resource = stored.get_allocator().resource();
    std::pmr::vector<stored_type> ordered(resource);
    ordered.reserve(handlers.size());
    for (const auto& registration : handlers) {
        ordered.emplace_back(resource, std::string_view(registration.first), callback_access::ref(registration.second));
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
    : plan_(make_pmr_object<compiled_route_plan>(process_resource())),
      route_hashes_(process_resource()),
      route_middleware_descriptors_(process_resource()),
      route_middleware_ranges_(process_resource()),
      global_middleware_descriptors_(process_resource()) {}

detail::router::~router() = default;

void detail::router::set_error_handler(http_error_handler_ref handler) noexcept {
    plan_->error_handler_ = handler;
}

void detail::router::set_not_found_handler(http_not_found_handler_ref handler) noexcept {
    plan_->not_found_handler_ = handler;
}

void detail::router::set_prefix_error_handlers(
    std::span<const std::pair<std::pmr::string, http_error_handler>> handlers) {
    replace_prefix_handlers(plan_->prefix_error_handlers_, handlers);
}

void detail::router::set_prefix_not_found_handlers(
    std::span<const std::pair<std::pmr::string, http_not_found_handler>> handlers) {
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
    routes.emplace_back(process_resource(),
        route_entry::init_type{.index_ = index,
            .controller_ = controller_slot_,
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
