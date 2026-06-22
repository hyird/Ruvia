#include "../RouterInternal.h"

#include "RouterUtils.h"
#include "ruvia/memory/PmrObject.h"

#include <stdexcept>

namespace ruvia {

using namespace detail;

Task<HttpResponse> Next::operator()(Context& context) const {
    if (invoke_ == nullptr) {
        throw std::logic_error("route continuation is empty");
    }
    return invoke_(target_, context);
}

void detail::RouterImplDeleter::operator()(RouterImpl* impl) const noexcept {
    destroyPmrObject(impl, startupResource());
}

void detail::RouterImpl::RouteTableDeleter::operator()(RouteTable* table) const noexcept {
    destroyPmrObject(table, resource == nullptr ? startupResource() : resource);
}

Router::Router()
    : impl_(
          constructPmrObject<detail::RouterImpl>(startupResource(), *this)) {}

Router::~Router() = default;

Router& detail::RouterImpl::setErrorHandler(HttpErrorHandler handler) noexcept {
    errorHandler_ = handler;
    if (routeTable_) {
        routeTable_->setErrorHandler(handler);
    }
    return owner;
}

void detail::RouteTable::setErrorHandler(HttpErrorHandler handler) noexcept {
    errorHandler_ = handler;
}

detail::RouterImpl::MiddlewareLifetime::MiddlewareLifetime(
    void* targetValue,
    RouteMiddleware::Destroy destroyValue) noexcept
    : target(targetValue), destroy(destroyValue) {}

detail::RouterImpl::MiddlewareLifetime::MiddlewareLifetime(MiddlewareLifetime&& other) noexcept
    : target(std::exchange(other.target, nullptr)),
      destroy(std::exchange(other.destroy, nullptr)) {}

detail::RouterImpl::MiddlewareLifetime& detail::RouterImpl::MiddlewareLifetime::operator=(
    MiddlewareLifetime&& other) noexcept {
    if (this == &other) {
        return *this;
    }

    reset();
    target = std::exchange(other.target, nullptr);
    destroy = std::exchange(other.destroy, nullptr);
    return *this;
}

detail::RouterImpl::MiddlewareLifetime::~MiddlewareLifetime() {
    reset();
}

void detail::RouterImpl::MiddlewareLifetime::reset() noexcept {
    if (target != nullptr && destroy != nullptr) {
        destroy(target);
    }
    target = nullptr;
    destroy = nullptr;
}

detail::RouteMiddleware detail::RouterImpl::materializeMiddleware(RouteMiddleware middleware) {
    if (middleware.target != nullptr) {
        middleware.create = nullptr;
        middleware.destroy = nullptr;
        return middleware;
    }
    if (middleware.create == nullptr || middleware.destroy == nullptr || middleware.invoke == nullptr) {
        throw std::invalid_argument("middleware must be constructible");
    }

    middleware.target = middleware.create();
    middlewareLifetimes_.emplace_back(middleware.target, middleware.destroy);
    middleware.create = nullptr;
    middleware.destroy = nullptr;
    return middleware;
}

void detail::RouterImpl::materializeMiddlewares(std::pmr::vector<RouteMiddleware>& middlewares) {
    for (auto& middleware : middlewares) {
        middleware = materializeMiddleware(std::move(middleware));
    }
}

void detail::RouterImpl::validateRouteTarget(HttpMethod method, std::string_view path) const {
    if (!RouteTable::isRoutableMethod(method)) {
        throw std::invalid_argument("route method must be routable");
    }

    for (const auto& route : pendingRoutes_) {
        if (route.method == method && route.path == path) {
            throw std::invalid_argument("duplicate route registration");
        }
    }
}

void detail::RouterImpl::finalize() {
    if (finalized_) {
        return;
    }

    validateNoDynamicRouteConflict(pendingRoutes_);
    routeTable_.reset(constructPmrObject<RouteTable>(startupResource(), buildRouteTable()));
    routeTable_.get_deleter().resource = startupResource();
    routeTable_->setErrorHandler(errorHandler_);
    finalized_ = true;
}

const detail::RouteTable& detail::RouterImpl::routeTable() const {
    if (!routeTable_) {
        throw std::logic_error("router has not been finalized");
    }
    return *routeTable_;
}

detail::RouteResolution detail::RouterImpl::resolve(const HttpRequest& request) const noexcept {
    if (!routeTable_) {
        return RouteResolution{};
    }
    return routeTable_->resolve(request);
}

}  // namespace ruvia
