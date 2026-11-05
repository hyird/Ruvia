#include "ruvia/web/detail/app/app_configuration.h"

#include <stdexcept>
#include <string_view>
#include <utility>

#include "ruvia/core/memory/pmr_object.h"

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
        if (fallback_prefixes_equivalent(existing.first, normalized)) {
            throw std::invalid_argument("duplicate fallback prefix");
        }
    }
    handlers.emplace_back(
        std::pmr::string(normalized, handlers.get_allocator().resource()), std::move(handler));
}

template <typename view_type, typename handlers_type, typename setter_type>
void apply_prefix_handlers(
    const handlers_type& handlers, std::pmr::memory_resource* resource, setter_type&& setter) {
    if (handlers.empty()) {
        return;
    }
    std::pmr::vector<view_type> views(resource);
    views.reserve(handlers.size());
    for (const auto& [prefix, handler] : handlers) {
        views.push_back({std::string_view(prefix), callback_access::ref(handler)});
    }
    setter(std::span<const view_type>(views));
}

}  // namespace

app_configuration::app_configuration(std::pmr::memory_resource* resource)
    : prefix_error_handlers_(pmr_resource_or_default(resource)),
      prefix_not_found_handlers_(pmr_resource_or_default(resource)),
      middlewares_(pmr_resource_or_default(resource)),
      worker_states_(pmr_resource_or_default(resource)) {}

void app_configuration::add_middleware(owned_middleware_descriptor descriptor) {
    const auto& value = descriptor.descriptor();
    if (value.validated_model_type_key() != nullptr) {
        throw std::invalid_argument("validator middleware binds to a route and cannot be app-wide");
    }
    // Route admission reads body-limit declarations from route and controller
    // lists only; app-wide they would silently bound nothing. The app-wide
    // ceiling is server_config::max_buffered_body_bytes_.
    if (value.request_body_limit() != 0) {
        throw std::invalid_argument("body_limit binds to a route or controller and cannot be app-wide");
    }
    if (value.stream_request_body()) {
        throw std::invalid_argument("stream_body binds to a route or controller and cannot be app-wide");
    }
    middlewares_.push_back(std::move(descriptor));
}

void app_configuration::add_worker_state(worker_state_definition definition) {
    append_worker_state_definition(worker_states_, std::move(definition));
}

// A second app-wide registration throws like a duplicate scoped prefix rather
// than silently replacing the first by call order.
void app_configuration::on_error(http_error_handler handler) {
    if (!handler) {
        throw std::invalid_argument("error handler must not be null");
    }
    if (error_handler_) {
        throw std::invalid_argument("duplicate app-wide error handler");
    }
    error_handler_ = std::move(handler);
}

http_error_handler app_configuration::on_error(std::nullptr_t) noexcept {
    return std::exchange(error_handler_, nullptr);
}

void app_configuration::on_error(scoped_error_handler_options options) {
    append_prefix_handler(prefix_error_handlers_, options.prefix_.view(), std::move(options.handler_));
}

void app_configuration::on_not_found(http_not_found_handler handler) {
    if (!handler) {
        throw std::invalid_argument("not found handler must not be null");
    }
    if (not_found_handler_) {
        throw std::invalid_argument("duplicate app-wide not found handler");
    }
    not_found_handler_ = std::move(handler);
}

http_not_found_handler app_configuration::on_not_found(std::nullptr_t) noexcept {
    return std::exchange(not_found_handler_, nullptr);
}

void app_configuration::on_not_found(scoped_not_found_handler_options options) {
    append_prefix_handler(prefix_not_found_handlers_, options.prefix_.view(), std::move(options.handler_));
}

void app_configuration::apply(
    router& routes_value, std::pmr::memory_resource* temporary_resource) const {
    auto* resource = pmr_resource_or_default(temporary_resource);
    routes_value.set_error_handler(callback_access::ref(error_handler_));
    routes_value.set_not_found_handler(callback_access::ref(not_found_handler_));
    apply_prefix_handlers<http_prefix_error_handler>(
        prefix_error_handlers_, resource, [&routes_value](std::span<const http_prefix_error_handler> views) {
            routes_value.set_prefix_error_handlers(views);
        });
    apply_prefix_handlers<http_prefix_not_found_handler>(prefix_not_found_handlers_, resource,
        [&routes_value](std::span<const http_prefix_not_found_handler> views) {
            routes_value.set_prefix_not_found_handlers(views);
        });
    if (!middlewares_.empty()) {
        std::pmr::vector<controller_middleware_descriptor> descriptors(resource);
        descriptors.reserve(middlewares_.size());
        for (const auto& middleware_value : middlewares_) {
            descriptors.push_back(middleware_value.descriptor());
        }
        routes_value.set_global_middlewares(descriptors);
    }
}

}  // namespace ruvia::detail
