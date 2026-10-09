#include "ruvia/web/detail/app/app_configuration.h"

#include <stdexcept>
#include <string_view>
#include <utility>

#include "ruvia/core/memory/pmr_resource.h"

#include "router/router_impl.h"

namespace ruvia::detail {
namespace {

template <typename handlers_type, typename handler_type>
void append_prefix_handler(handlers_type& handlers, std::string_view prefix, handler_type handler) {
    if (!handler) {
        throw std::invalid_argument("fallback handler must not be null");
    }
    const auto normalized = normalize_fallback_prefix(prefix);
    for (const auto& existing : handlers) {
        if (std::string_view(existing.first) == normalized) {
            throw std::invalid_argument("duplicate fallback prefix");
        }
    }
    handlers.emplace_back(
        std::pmr::string(normalized, handlers.get_allocator().resource()), std::move(handler));
}

template <typename view_type, typename handlers_type>
[[nodiscard]] std::pmr::vector<view_type> prefix_handler_views(
    const handlers_type& handlers, std::pmr::memory_resource* resource) {
    std::pmr::vector<view_type> views(resource);
    views.reserve(handlers.size());
    for (const auto& [prefix, handler] : handlers) {
        views.push_back({std::string_view(prefix), callback_access::ref(handler)});
    }
    return views;
}

}  // namespace

app_configuration::app_configuration(std::pmr::memory_resource* resource)
    : prefix_error_handlers_(pmr_resource_or_default(resource)),
      prefix_not_found_handlers_(pmr_resource_or_default(resource)),
      middlewares_(pmr_resource_or_default(resource)),
      worker_states_(pmr_resource_or_default(resource)) {}

void app_configuration::add_middleware(controller_middleware_descriptor descriptor) {
    if (!descriptor.valid() || descriptor.create() == nullptr || descriptor.destroy() == nullptr) {
        throw std::invalid_argument("app middleware must be constructible and invocable");
    }
    if (descriptor.validated_model_type_key() != nullptr) {
        throw std::invalid_argument("validator middleware binds to a route and cannot be app-wide");
    }
    middlewares_.push_back(descriptor);
}

void app_configuration::add_worker_state(worker_state_definition definition) {
    append_worker_state_definition(worker_states_, std::move(definition));
}

void app_configuration::on_error(http_error_handler_type handler) {
    error_handler_ = std::move(handler);
}

void app_configuration::on_error(scoped_error_handler_options options) {
    append_prefix_handler(prefix_error_handlers_, options.prefix_, std::move(options.handler_));
}

void app_configuration::on_not_found(http_not_found_handler_type handler) {
    not_found_handler_ = std::move(handler);
}

void app_configuration::on_not_found(scoped_not_found_handler_options options) {
    append_prefix_handler(prefix_not_found_handlers_, options.prefix_, std::move(options.handler_));
}

void app_configuration::apply(
    router_impl& routes_value, std::pmr::memory_resource* temporary_resource) const {
    auto* resource = pmr_resource_or_default(temporary_resource);
    routes_value.set_error_handler(callback_access::ref(error_handler_));
    routes_value.set_not_found_handler(callback_access::ref(not_found_handler_));
    if (!prefix_error_handlers_.empty()) {
        const auto views = prefix_handler_views<http_prefix_error_handler>(prefix_error_handlers_, resource);
        routes_value.set_prefix_error_handlers(views);
    }
    if (!prefix_not_found_handlers_.empty()) {
        const auto views = prefix_handler_views<http_prefix_not_found_handler>(prefix_not_found_handlers_, resource);
        routes_value.set_prefix_not_found_handlers(views);
    }
    if (!middlewares_.empty()) {
        routes_value.set_global_middlewares(middlewares_);
    }
}

}  // namespace ruvia::detail
