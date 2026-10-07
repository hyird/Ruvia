#include "ruvia/web/detail/app/app_configuration.h"

#include <stdexcept>
#include <string_view>
#include <utility>

#include "ruvia/core/memory/PmrResource.h"
#include "ruvia/web/detail/router/RouterImpl.h"

namespace ruvia::detail {
namespace {

template <typename handlers_type, typename handler_type>
void append_prefix_handler(handlers_type& handlers, std::string_view prefix, handler_type handler) {
    if (!handler) {
        throw std::invalid_argument("fallback handler must not be null");
    }
    const auto normalized = normalizeFallbackPrefix(prefix);
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
        views.push_back({std::string_view(prefix), CallbackAccess::ref(handler)});
    }
    return views;
}

}  // namespace

app_configuration::app_configuration(std::pmr::memory_resource* resource)
    : prefix_error_handlers_(pmrResourceOrDefault(resource)),
      prefix_not_found_handlers_(pmrResourceOrDefault(resource)),
      middlewares_(pmrResourceOrDefault(resource)),
      worker_states_(pmrResourceOrDefault(resource)) {}

void app_configuration::add_middleware(ControllerMiddlewareDescriptor descriptor) {
    if (!descriptor.valid() || descriptor.create() == nullptr || descriptor.destroy() == nullptr) {
        throw std::invalid_argument("app middleware must be constructible and invocable");
    }
    if (descriptor.validatedModelTypeKey() != nullptr) {
        throw std::invalid_argument("validator middleware binds to a route and cannot be app-wide");
    }
    middlewares_.push_back(descriptor);
}

void app_configuration::add_worker_state(WorkerStateDefinition definition) {
    appendWorkerStateDefinition(worker_states_, std::move(definition));
}

void app_configuration::on_error(HttpErrorHandler handler) {
    error_handler_ = std::move(handler);
}

void app_configuration::on_error(ScopedErrorHandlerOptions options) {
    append_prefix_handler(prefix_error_handlers_, options.prefix, std::move(options.handler));
}

void app_configuration::on_not_found(HttpNotFoundHandler handler) {
    not_found_handler_ = std::move(handler);
}

void app_configuration::on_not_found(ScopedNotFoundHandlerOptions options) {
    append_prefix_handler(prefix_not_found_handlers_, options.prefix, std::move(options.handler));
}

void app_configuration::apply(
    RouterImpl& routes, std::pmr::memory_resource* temporary_resource) const {
    auto* resource = pmrResourceOrDefault(temporary_resource);
    routes.setErrorHandler(CallbackAccess::ref(error_handler_));
    routes.setNotFoundHandler(CallbackAccess::ref(not_found_handler_));
    if (!prefix_error_handlers_.empty()) {
        const auto views = prefix_handler_views<HttpPrefixErrorHandler>(prefix_error_handlers_, resource);
        routes.setPrefixErrorHandlers(views);
    }
    if (!prefix_not_found_handlers_.empty()) {
        const auto views = prefix_handler_views<HttpPrefixNotFoundHandler>(prefix_not_found_handlers_, resource);
        routes.setPrefixNotFoundHandlers(views);
    }
    if (!middlewares_.empty()) {
        routes.setGlobalMiddlewares(middlewares_);
    }
}

}  // namespace ruvia::detail
