#include <algorithm>
#include <stdexcept>
#include <string_view>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/http/http_request_target.h"
#include "ruvia/web/detail/app/app_configuration.h"
#include "ruvia/web/detail/router/prefix_fallback.h"
#include "ruvia/web/detail/util/registration_resource.h"

#include "router/router_impl.h"

namespace ruvia {

using namespace detail;

namespace {

template <typename stored_type, typename registration_type>
void replace_prefix_handlers(std::pmr::vector<stored_type>& stored, std::span<const registration_type> handlers) {
    auto* const resource = stored.get_allocator().resource();
    std::pmr::vector<stored_type> normalized(resource);
    normalized.reserve(handlers.size());
    for (const auto& registration : handlers) {
        if (registration.handler_ == nullptr) {
            throw std::invalid_argument("fallback handler must not be null");
        }
        const auto prefix = detail::normalize_fallback_prefix(registration.prefix_);
        for (const auto& existing : normalized) {
            if (detail::fallback_prefixes_equivalent(existing.prefix_, prefix)) {
                throw std::invalid_argument("duplicate fallback prefix");
            }
        }
        normalized.emplace_back(resource, prefix, registration.handler_);
    }
    // Deepest prefix first: selection is a first-match scan. A nested scope
    // always has more segments; distinct scopes of equal depth cannot nest, so
    // their relative order is irrelevant; keep it stable.
    std::ranges::stable_sort(normalized, [](const stored_type& left, const stored_type& right) noexcept {
        return detail::fallback_prefix_depth(left.prefix_) > detail::fallback_prefix_depth(right.prefix_);
    });
    stored = std::move(normalized);
}

void validate_middleware_descriptor(const controller_middleware_descriptor& descriptor) {
    if (!descriptor.valid()) {
        throw std::invalid_argument("middleware must be invocable");
    }
    if (descriptor.create() == nullptr || descriptor.destroy() == nullptr) {
        throw std::invalid_argument("middleware must be constructible");
    }
}

}  // namespace

detail::router_impl::router_impl(router& router_value, std::pmr::memory_resource* plan_resource)
    : owner_(router_value),
      plan_(make_pmr_object<compiled_route_plan>(plan_resource, plan_resource)),
      route_indices_(registration_resource()),
      route_middleware_descriptors_(registration_resource()),
      route_middleware_ranges_(registration_resource()),
      global_middleware_descriptors_(registration_resource()) {}

void router::impl_deleter_type::operator()(detail::router_impl* impl) const noexcept {
    destroy_pmr_object(impl, registration_resource());
}

router::router(std::pmr::memory_resource* plan_resource)
    : impl_(construct_pmr_object<detail::router_impl>(registration_resource(), *this, plan_resource)) {}

router::~router() = default;

void detail::router_impl::set_error_handler(http_error_handler_ref_type handler) noexcept {
    plan_->error_handler_ = handler;
}

void detail::router_impl::set_not_found_handler(http_not_found_handler_ref_type handler) noexcept {
    plan_->not_found_handler_ = handler;
}

void detail::router_impl::set_prefix_error_handlers(std::span<const http_prefix_error_handler> handlers) {
    replace_prefix_handlers(plan_->prefix_error_handlers_, handlers);
}

void detail::router_impl::set_prefix_not_found_handlers(
    std::span<const http_prefix_not_found_handler> handlers) {
    replace_prefix_handlers(plan_->prefix_not_found_handlers_, handlers);
}

void detail::router_impl::set_global_middlewares(std::span<const controller_middleware_descriptor> descriptors) {
    for (const auto& descriptor : descriptors) {
        validate_middleware_descriptor(descriptor);
        plan_->has_route_rate_limit_ = plan_->has_route_rate_limit_ || descriptor.uses_route_rate_limit();
    }
    global_middleware_descriptors_.assign(descriptors.begin(), descriptors.end());
}

void detail::router_impl::append_route_middlewares(
    std::span<const controller_middleware_descriptor> controller_middlewares,
    std::span<const controller_middleware_descriptor> route_middlewares) {
    const auto offset = route_middleware_descriptors_.size();
    route_middleware_descriptors_.reserve(offset + controller_middlewares.size() + route_middlewares.size());
    for (const auto middlewares : {controller_middlewares, route_middlewares}) {
        for (const auto& descriptor : middlewares) {
            validate_middleware_descriptor(descriptor);
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
}

void detail::router_impl::declare_controllers(
    std::span<const controller_registrar_type> registrars, const controller_store& controllers) {
    if (controllers.size() != registrars.size()) {
        throw std::logic_error("controller instances do not match the controller registrars");
    }
    for (std::size_t slot = 0; slot < registrars.size(); ++slot) {
        controller_slot_ = slot;
        controller_target_ = controllers.instance(slot);
        registrars[slot].register_routes_(controller_target_, owner_);
    }
    controller_target_ = nullptr;
    plan_->controller_count_ = registrars.size();
}

void detail::router_impl::validate_route_target(
    http_known_method method, std::string_view method_token, std::string_view path, const route_endpoint& endpoint) const {
    if (const auto* tunnel = endpoint.tunnel()) {
        if (method != http_known_method::connect || !method_token.empty()) {
            throw std::invalid_argument("tunnel route requires CONNECT");
        }
        if (tunnel->protocol().empty()) {
            if (path != "*" && !is_valid_http_connect_authority(path)) {
                throw std::invalid_argument("CONNECT route requires host:port authority or *");
            }
        } else if ((path.find('?') != std::string_view::npos) || !is_valid_http_origin_form_target(path)) {
            throw std::invalid_argument("extended CONNECT route requires an origin-form path");
        }
        return;
    }
    // An extension route carries unknown plus a token; anything else must sit
    // in the enum-indexed fast path.
    if (method_token.empty() && !route_table::is_routable_method(method)) {
        throw std::invalid_argument("route method must be routable");
    }
    // Extension routes are matched by a linear scan over a cold list, which has
    // no dynamic-segment index behind it.
    if (!method_token.empty() && is_dynamic_path(path)) {
        throw std::invalid_argument("extension method routes must use a static path");
    }
    if ((path.find('?') != std::string_view::npos) || !ruvia::is_valid_http_origin_form_target(path)) {
        throw std::invalid_argument("route path must be an origin-form path without query");
    }
}

detail::compiled_route_plan_ptr_type detail::router_impl::compile() {
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
    build_allowed_method_mask();
    build_static_index();
    build_dynamic_routes();
    build_server_extension_method_tokens();
    build_url_patterns();
    return std::move(plan_);
}

detail::compiled_route_plan_ptr_type detail::compile_route_plan(std::pmr::memory_resource* resource,
    std::span<const controller_registrar_type> registrars, const controller_store& controllers,
    const app_configuration& configuration) {
    router router_value(resource);
    auto& routes_value = router_impl::from(router_value);
    routes_value.declare_controllers(registrars, controllers);
    configuration.apply(routes_value, resource);
    return routes_value.compile();
}

}  // namespace ruvia
