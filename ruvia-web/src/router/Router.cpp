#include "ruvia/web/detail/router/RouterInternal.h"

#include "ruvia/web/detail/RegistrationResource.h"
#include "ruvia/core/memory/PmrObject.h"

#include <algorithm>
#include <stdexcept>

namespace ruvia {

using namespace detail;

namespace {

Task<void> ignoreExpiredNext(NextState) {
    co_return;
}

void validateUniqueValidatedModelTypes(
    std::span<const ControllerMiddlewareDescriptor> descriptors) {
    for (std::size_t i = 0; i < descriptors.size(); ++i) {
        const auto* const key = descriptors[i].validatedModelTypeKey();
        if (key == nullptr) {
            continue;
        }
        for (std::size_t j = i + 1; j < descriptors.size(); ++j) {
            if (descriptors[j].validatedModelTypeKey() == key) {
                throw std::invalid_argument("duplicate validated model type on route");
            }
        }
    }
}

void validateUniqueValidatedModelTypes(
    std::span<const ControllerMiddlewareDescriptor> first,
    std::span<const ControllerMiddlewareDescriptor> second) {
    validateUniqueValidatedModelTypes(first);
    validateUniqueValidatedModelTypes(second);
    for (const auto& left : first) {
        const auto* const key = left.validatedModelTypeKey();
        if (key == nullptr) {
            continue;
        }
        for (const auto& right : second) {
            if (right.validatedModelTypeKey() == key) {
                throw std::invalid_argument("duplicate validated model type on route");
            }
        }
    }
}

[[nodiscard]] bool usesRouteRateLimit(
    std::span<const ControllerMiddlewareDescriptor> descriptors) noexcept {
    for (const auto& descriptor : descriptors) {
        if (descriptor.usesRouteRateLimit()) {
            return true;
        }
    }
    return false;
}

}  // namespace

Next::Awaitable Next::operator()() & {
    auto state = state_;
    auto* control = state.control;
    state.invocation = control == nullptr
        ? detail::NextState::Invocation::kExpired
        : control->beginInvocation();
    if (state.invocation == detail::NextState::Invocation::kExpired) {
        return Awaitable(state, &ignoreExpiredNext);
    }
    return Awaitable(state, invoke_);
}

detail::RouterImpl::RouterImpl(Router& router) noexcept
    : owner(router),
      resource_(registrationResource()),
      pendingRoutes_(resource_),
      middlewareLifetimes_(resource_),
      globalMiddlewareDescriptors_(resource_),
      globalMiddlewareFrames_(resource_),
      routeTable_(nullptr, RouteTableDeleter{resource_}) {}

void Router::ImplDeleter::operator()(detail::RouterImpl* impl) const noexcept {
    destroyPmrObject(impl, registrationResource());
}

void detail::RouterImpl::RouteTableDeleter::operator()(RouteTable* table) const noexcept {
    destroyPmrObject(table, registrationResourceOrDefault(resource));
}

Router::Router()
    : impl_(
          constructPmrObject<detail::RouterImpl>(registrationResource(), *this)) {}

Router::~Router() = default;

Router& detail::RouterImpl::setErrorHandler(HttpErrorHandler handler) noexcept {
    errorHandler_ = handler;
    if (routeTable_) {
        routeTable_->setErrorHandler(handler);
    }
    return owner;
}

Router& detail::RouterImpl::setNotFoundHandler(HttpNotFoundHandler handler) noexcept {
    notFoundHandler_ = handler;
    if (routeTable_) {
        routeTable_->setNotFoundHandler(handler);
    }
    return owner;
}

void detail::RouteTable::setErrorHandler(HttpErrorHandler handler) noexcept {
    errorHandler_ = handler;
}

void detail::RouteTable::setNotFoundHandler(HttpNotFoundHandler handler) noexcept {
    notFoundHandler_ = handler;
}

namespace {

// Normalizes one prefix registration ("/api/" and "/api" are the same scope)
// and validates the shape shared by both fallback kinds. "/" stays "/" and
// scopes every path; plain setErrorHandler/setNotFoundHandler remain the
// simpler spelling for that.
[[nodiscard]] std::string_view normalizeFallbackPrefix(std::string_view prefix) {
    if (prefix.empty() || prefix.front() != '/') {
        throw std::invalid_argument("fallback prefix must start with '/'");
    }
    while (prefix.size() > 1 && prefix.back() == '/') {
        prefix.remove_suffix(1);
    }
    return prefix;
}

template <typename Stored, typename Registration>
void replacePrefixHandlers(
    std::pmr::vector<Stored>& stored,
    std::pmr::memory_resource* resource,
    std::span<const Registration> handlers) {
    std::pmr::vector<Stored> normalized(resource);
    normalized.reserve(handlers.size());
    for (const auto& registration : handlers) {
        if (registration.handler == nullptr) {
            throw std::invalid_argument("fallback handler must not be null");
        }
        const auto prefix = normalizeFallbackPrefix(registration.prefix);
        for (const auto& existing : normalized) {
            if (std::string_view(existing.prefix) == prefix) {
                throw std::invalid_argument("duplicate fallback prefix");
            }
        }
        normalized.emplace_back(resource, prefix, registration.handler);
    }
    // Longest prefix first: selection is a first-match scan. Equal lengths
    // cannot nest, so their relative order is irrelevant; keep it stable.
    std::stable_sort(
        normalized.begin(),
        normalized.end(),
        [](const Stored& left, const Stored& right) noexcept {
            return left.prefix.size() > right.prefix.size();
        });
    stored = std::move(normalized);
}

// Longest-first stored order: the first hit is the tightest scope. A prefix
// matches on whole path segments only, so "/api" scopes "/api" and "/api/x"
// but never "/apix".
template <typename Stored>
[[nodiscard]] auto selectPrefixHandler(
    const std::pmr::vector<Stored>& stored,
    std::string_view path) noexcept {
    for (const auto& candidate : stored) {
        const std::string_view prefix(candidate.prefix);
        if (prefix == "/" || path == prefix ||
            (path.size() > prefix.size() && path.starts_with(prefix) &&
             path[prefix.size()] == '/')) {
            return candidate.handler;
        }
    }
    return decltype(stored.front().handler){nullptr};
}

}  // namespace

void detail::RouteTable::setPrefixErrorHandlers(
    std::span<const HttpPrefixErrorHandler> handlers) {
    replacePrefixHandlers(prefixErrorHandlers_, resource_, handlers);
}

void detail::RouteTable::setPrefixNotFoundHandlers(
    std::span<const HttpPrefixNotFoundHandler> handlers) {
    replacePrefixHandlers(prefixNotFoundHandlers_, resource_, handlers);
}

HttpErrorHandler detail::RouteTable::errorHandlerFor(
    std::string_view path) const noexcept {
    const auto handler = selectPrefixHandler(prefixErrorHandlers_, path);
    return handler != nullptr ? handler : errorHandler_;
}

HttpNotFoundHandler detail::RouteTable::notFoundHandlerFor(
    std::string_view path) const noexcept {
    const auto handler = selectPrefixHandler(prefixNotFoundHandlers_, path);
    return handler != nullptr ? handler : notFoundHandler_;
}

Router& detail::RouterImpl::setPrefixErrorHandlers(
    std::span<const HttpPrefixErrorHandler> handlers) {
    if (routeTable_) {
        routeTable_->setPrefixErrorHandlers(handlers);
    }
    prefixErrorHandlers_.clear();
    prefixErrorHandlers_.reserve(handlers.size());
    for (const auto& handler : handlers) {
        prefixErrorHandlers_.emplace_back(
            std::pmr::string(handler.prefix, resource_), handler.handler);
    }
    return owner;
}

Router& detail::RouterImpl::setPrefixNotFoundHandlers(
    std::span<const HttpPrefixNotFoundHandler> handlers) {
    if (routeTable_) {
        routeTable_->setPrefixNotFoundHandlers(handlers);
    }
    prefixNotFoundHandlers_.clear();
    prefixNotFoundHandlers_.reserve(handlers.size());
    for (const auto& handler : handlers) {
        prefixNotFoundHandlers_.emplace_back(
            std::pmr::string(handler.prefix, resource_), handler.handler);
    }
    return owner;
}

detail::RouterImpl::MiddlewareLifetime::MiddlewareLifetime(
    void* targetValue,
    ControllerMiddlewareDescriptor::Destroy destroyValue) noexcept
    : target_(targetValue), destroy_(destroyValue) {}

detail::RouterImpl::MiddlewareLifetime::MiddlewareLifetime(MiddlewareLifetime&& other) noexcept
    : target_(std::exchange(other.target_, nullptr)),
      destroy_(std::exchange(other.destroy_, nullptr)) {}

detail::RouterImpl::MiddlewareLifetime& detail::RouterImpl::MiddlewareLifetime::operator=(
    MiddlewareLifetime&& other) noexcept {
    if (this == &other) {
        return *this;
    }

    reset();
    target_ = std::exchange(other.target_, nullptr);
    destroy_ = std::exchange(other.destroy_, nullptr);
    return *this;
}

detail::RouterImpl::MiddlewareLifetime::~MiddlewareLifetime() {
    reset();
}

void detail::RouterImpl::MiddlewareLifetime::reset() noexcept {
    if (target_ != nullptr && destroy_ != nullptr) {
        destroy_(target_);
    }
    target_ = nullptr;
    destroy_ = nullptr;
}

detail::RouteMiddleware detail::RouterImpl::materializeMiddleware(ControllerMiddlewareDescriptor middleware) {
    if (!middleware.valid()) {
        throw std::invalid_argument("middleware must be invocable");
    }
    if (middleware.create() == nullptr || middleware.destroy() == nullptr) {
        throw std::invalid_argument("middleware must be constructible");
    }

    auto* target = middleware.create()();
    middlewareLifetimes_.emplace_back(target, middleware.destroy());
    return RouteMiddleware(target, middleware.invoke());
}

void detail::RouterImpl::appendMaterializedMiddlewares(
    std::pmr::vector<RouteMiddleware>& frames,
    std::span<const ControllerMiddlewareDescriptor> descriptors) {
    for (const auto& middleware : descriptors) {
        frames.push_back(materializeMiddleware(middleware));
    }
}

std::pmr::vector<detail::RouteMiddleware> detail::RouterImpl::materializeMiddlewares(
    std::span<const ControllerMiddlewareDescriptor> first,
    std::span<const ControllerMiddlewareDescriptor> second) {
    validateUniqueValidatedModelTypes(first, second);
    hasRouteRateLimit_ = hasRouteRateLimit_ ||
        usesRouteRateLimit(first) || usesRouteRateLimit(second);
    std::pmr::vector<RouteMiddleware> frames(resource_);
    frames.reserve(first.size() + second.size());
    appendMaterializedMiddlewares(frames, first);
    appendMaterializedMiddlewares(frames, second);
    return frames;
}

void detail::RouterImpl::validateRouteTarget(HttpKnownMethod method, std::string_view path) const {
    if (!RouteTable::isRoutableMethod(method)) {
        throw std::invalid_argument("route method must be routable");
    }

    for (const auto& route : pendingRoutes_) {
        if (route.method() == method && route.path() == path) {
            throw std::invalid_argument("duplicate route registration");
        }
    }
}

void detail::RouterImpl::setGlobalMiddlewares(
    std::span<const ControllerMiddlewareDescriptor> descriptors) {
    if (routeTable_) {
        // A finalized table's middleware ranges are immutable. Re-applying the
        // identical set (an app stop()/run() cycle) is a no-op; changing it
        // requires a fresh router.
        const bool unchanged =
            descriptors.size() == globalMiddlewareDescriptors_.size() &&
            std::equal(
                descriptors.begin(),
                descriptors.end(),
                globalMiddlewareDescriptors_.begin(),
                [](const auto& left, const auto& right) noexcept {
                    return left.invoke() == right.invoke() &&
                        left.create() == right.create() &&
                        left.destroy() == right.destroy();
                });
        if (unchanged) {
            return;
        }
        throw std::logic_error("cannot change app middleware after router finalize");
    }
    globalMiddlewareDescriptors_.assign(descriptors.begin(), descriptors.end());
}

void detail::RouterImpl::finalize() {
    if (routeTable_) {
        return;
    }

    globalMiddlewareFrames_ = materializeMiddlewares(globalMiddlewareDescriptors_);
    validateNoDynamicRouteConflict(pendingRoutes_);
    std::unique_ptr<RouteTable, RouteTableDeleter> table(
        constructPmrObject<RouteTable>(resource_, resource_),
        RouteTableDeleter{resource_});
    buildRouteTable(*table);
    table->hasRouteRateLimit_ = hasRouteRateLimit_;
    table->setErrorHandler(errorHandler_);
    table->setNotFoundHandler(notFoundHandler_);
    if (!prefixErrorHandlers_.empty()) {
        std::pmr::vector<HttpPrefixErrorHandler> views(resource_);
        views.reserve(prefixErrorHandlers_.size());
        for (const auto& [prefix, handler] : prefixErrorHandlers_) {
            views.push_back({std::string_view(prefix), handler});
        }
        table->setPrefixErrorHandlers(views);
    }
    if (!prefixNotFoundHandlers_.empty()) {
        std::pmr::vector<HttpPrefixNotFoundHandler> views(resource_);
        views.reserve(prefixNotFoundHandlers_.size());
        for (const auto& [prefix, handler] : prefixNotFoundHandlers_) {
            views.push_back({std::string_view(prefix), handler});
        }
        table->setPrefixNotFoundHandlers(views);
    }
    routeTable_ = std::move(table);
}

const detail::RouteTable& detail::RouterImpl::routeTable() const {
    if (!routeTable_) {
        throw std::logic_error("router has not been finalized");
    }
    return *routeTable_;
}

}  // namespace ruvia
