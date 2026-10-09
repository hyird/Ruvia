#include <algorithm>
#include <stdexcept>
#include <string_view>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/http/http_request_target.h"
#include "ruvia/web/detail/util/registration_resource.h"

#include "router/router_impl.h"

namespace ruvia {

using namespace detail;

namespace {

task<void> ignore_expired_next(next_state) {
    co_return;
}

void validate_unique_validated_model_types(
    std::span<const controller_middleware_descriptor> descriptors) {
    for (std::size_t i = 0; i < descriptors.size(); ++i) {
        const auto* const key = descriptors[i].validated_model_type_key();
        if (key == nullptr) {
            continue;
        }
        for (std::size_t j = i + 1; j < descriptors.size(); ++j) {
            if (descriptors[j].validated_model_type_key() == key) {
                throw std::invalid_argument("duplicate validated model type on route");
            }
        }
    }
}

void validate_unique_validated_model_types(std::span<const controller_middleware_descriptor> first,
    std::span<const controller_middleware_descriptor> second) {
    validate_unique_validated_model_types(first);
    validate_unique_validated_model_types(second);
    for (const auto& left : first) {
        const auto* const key = left.validated_model_type_key();
        if (key == nullptr) {
            continue;
        }
        for (const auto& right : second) {
            if (right.validated_model_type_key() == key) {
                throw std::invalid_argument("duplicate validated model type on route");
            }
        }
    }
}

[[nodiscard]] bool uses_route_rate_limit(
    std::span<const controller_middleware_descriptor> descriptors) noexcept {
    return std::ranges::any_of(descriptors,
        [](const auto& descriptor) noexcept { return descriptor.uses_route_rate_limit(); });
}

}  // namespace

next::awaitable_type next::operator()() & {
    auto state_value = state_;
    auto* control = state_value.control_;
    state_value.invocation_ =
        control == nullptr ? detail::next_state::invocation_type::expired : control->begin_invocation();
    if (state_value.invocation_ == detail::next_state::invocation_type::expired) {
        return awaitable_type(state_value, &ignore_expired_next);
    }
    return awaitable_type(state_value, invoke_);
}

detail::router_impl::router_impl(router& router_value)
    : owner_(router_value),
      resource_(registration_resource()),
      pending_routes_(resource_),
      pending_route_indices_(std::in_place, resource_),
      middleware_lifetimes_(resource_),
      global_middleware_descriptors_(resource_),
      global_middleware_frames_(resource_),
      route_table_(nullptr, route_table_deleter_type{resource_}) {}

void router::impl_deleter_type::operator()(detail::router_impl* impl) const noexcept {
    destroy_pmr_object(impl, registration_resource());
}

void detail::router_impl::route_table_deleter_type::operator()(detail::route_table* table_value) const noexcept {
    destroy_pmr_object(table_value, registration_resource_or_default(resource_));
}

router::router()
    : impl_(construct_pmr_object<detail::router_impl>(registration_resource(), *this)) {}

router::~router() = default;

router& detail::router_impl::set_error_handler(http_error_handler_ref_type handler) noexcept {
    error_handler_ = handler;
    if (route_table_) {
        route_table_->set_error_handler(handler);
    }
    return owner_;
}

router& detail::router_impl::set_not_found_handler(http_not_found_handler_ref_type handler) noexcept {
    not_found_handler_ = handler;
    if (route_table_) {
        route_table_->set_not_found_handler(handler);
    }
    return owner_;
}

router& detail::router_impl::set_prefix_error_handlers(
    std::span<const http_prefix_error_handler> handlers) {
    if (route_table_) {
        route_table_->set_prefix_error_handlers(handlers);
    }
    prefix_error_handlers_.clear();
    prefix_error_handlers_.reserve(handlers.size());
    for (const auto& handler : handlers) {
        prefix_error_handlers_.emplace_back(
            std::pmr::string(handler.prefix_, resource_), handler.handler_);
    }
    return owner_;
}

router& detail::router_impl::set_prefix_not_found_handlers(
    std::span<const http_prefix_not_found_handler> handlers) {
    if (route_table_) {
        route_table_->set_prefix_not_found_handlers(handlers);
    }
    prefix_not_found_handlers_.clear();
    prefix_not_found_handlers_.reserve(handlers.size());
    for (const auto& handler : handlers) {
        prefix_not_found_handlers_.emplace_back(
            std::pmr::string(handler.prefix_, resource_), handler.handler_);
    }
    return owner_;
}

detail::router_impl::middleware_lifetime_type::middleware_lifetime_type(
    void* target_value, controller_middleware_descriptor::destroy_type destroy_value) noexcept
    : target_(target_value),
      destroy_(destroy_value) {}

detail::router_impl::middleware_lifetime_type::middleware_lifetime_type(middleware_lifetime_type&& other) noexcept
    : target_(std::exchange(other.target_, nullptr)),
      destroy_(std::exchange(other.destroy_, nullptr)) {}

detail::router_impl::middleware_lifetime_type& detail::router_impl::middleware_lifetime_type::operator=(
    middleware_lifetime_type&& other) noexcept {
    if (this == &other) {
        return *this;
    }

    reset();
    target_ = std::exchange(other.target_, nullptr);
    destroy_ = std::exchange(other.destroy_, nullptr);
    return *this;
}

detail::router_impl::middleware_lifetime_type::~middleware_lifetime_type() {
    reset();
}

void detail::router_impl::middleware_lifetime_type::reset() noexcept {
    if (target_ != nullptr && destroy_ != nullptr) {
        destroy_(target_);
    }
    target_ = nullptr;
    destroy_ = nullptr;
}

detail::route_middleware_type detail::router_impl::materialize_middleware(
    controller_middleware_descriptor middleware_value) {
    if (!middleware_value.valid()) {
        throw std::invalid_argument("middleware must be invocable");
    }
    if (middleware_value.create() == nullptr || middleware_value.destroy() == nullptr) {
        throw std::invalid_argument("middleware must be constructible");
    }

    auto* target = middleware_value.create()(middleware_value.args());
    middleware_lifetimes_.emplace_back(target, middleware_value.destroy());
    return route_middleware_type(target, middleware_value.invoke());
}

void detail::router_impl::append_materialized_middlewares(std::pmr::vector<route_middleware_type>& frames,
    std::span<const controller_middleware_descriptor> descriptors) {
    for (const auto& middleware : descriptors) {
        frames.push_back(materialize_middleware(middleware));
    }
}

std::pmr::vector<detail::route_middleware_type> detail::router_impl::materialize_middlewares(
    std::span<const controller_middleware_descriptor> first,
    std::span<const controller_middleware_descriptor> second) {
    validate_unique_validated_model_types(first, second);
    has_route_rate_limit_ =
        has_route_rate_limit_ || uses_route_rate_limit(first) || uses_route_rate_limit(second);
    std::pmr::vector<route_middleware_type> frames(resource_);
    frames.reserve(first.size() + second.size());
    append_materialized_middlewares(frames, first);
    append_materialized_middlewares(frames, second);
    return frames;
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
    if (!method_token.empty() && route_table::is_dynamic_path(path)) {
        throw std::invalid_argument("extension method routes must use a static path");
    }
    if ((path.find('?') != std::string_view::npos) || !ruvia::is_valid_http_origin_form_target(path)) {
        throw std::invalid_argument("route path must be an origin-form path without query");
    }
}

void detail::router_impl::set_global_middlewares(
    std::span<const controller_middleware_descriptor> descriptors) {
    if (route_table_) {
        // A finalized table's middleware ranges are immutable. Re-applying the
        // identical set (an app stop()/run() cycle) is a no-op; changing it
        // requires a fresh router.
        const bool unchanged = descriptors.size() == global_middleware_descriptors_.size() &&
                               std::ranges::equal(descriptors, global_middleware_descriptors_);
        if (unchanged) {
            return;
        }
        throw std::logic_error("cannot change app middleware after router finalize");
    }
    global_middleware_descriptors_.assign(descriptors.begin(), descriptors.end());
}

void detail::router_impl::finalize(const compiled_route_plan* compiled_plan) {
    if (route_table_) {
        return;
    }

    global_middleware_frames_ = materialize_middlewares(global_middleware_descriptors_);
    validate_no_dynamic_route_conflict(pending_routes_);
    std::unique_ptr<detail::route_table, route_table_deleter_type> table_value(
        construct_pmr_object<detail::route_table>(resource_, resource_), route_table_deleter_type{resource_});
    table_value->has_route_rate_limit_ = has_route_rate_limit_;
    build_route_table(*table_value, compiled_plan);
    table_value->set_error_handler(error_handler_);
    table_value->set_not_found_handler(not_found_handler_);
    if (!prefix_error_handlers_.empty()) {
        std::pmr::vector<http_prefix_error_handler> views(resource_);
        views.reserve(prefix_error_handlers_.size());
        for (const auto& [prefix, handler] : prefix_error_handlers_) {
            views.push_back({std::string_view(prefix), handler});
        }
        table_value->set_prefix_error_handlers(views);
    }
    if (!prefix_not_found_handlers_.empty()) {
        std::pmr::vector<http_prefix_not_found_handler> views(resource_);
        views.reserve(prefix_not_found_handlers_.size());
        for (const auto& [prefix, handler] : prefix_not_found_handlers_) {
            views.push_back({std::string_view(prefix), handler});
        }
        table_value->set_prefix_not_found_handlers(views);
    }
    route_table_ = std::move(table_value);
    pending_route_indices_.reset();
    std::pmr::vector<pending_route_type>(resource_).swap(pending_routes_);
}

const detail::route_table& detail::router_impl::route_table() const {
    if (!route_table_) {
        throw std::logic_error("router has not been finalized");
    }
    return *route_table_;
}

detail::compiled_route_plan_ptr_type detail::router_impl::release_compiled_plan() {
    if (!route_table_) {
        throw std::logic_error("router has not been finalized");
    }
    return route_table_->release_compiled_plan();
}

}  // namespace ruvia
