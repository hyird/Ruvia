#include "../RouterInternal.h"

#include "ruvia/http/detail/RegistrationResource.h"
#include "ruvia/memory/PmrObject.h"

#include <stdexcept>

namespace ruvia {

using namespace detail;

Task<HttpResponse> Next::operator()(Context& context) const {
    return callable_(context);
}

detail::RouterImpl::RouterImpl(Router& router) noexcept
    : owner(router),
      routeTable_(nullptr, RouteTableDeleter{registrationResource()}) {}

void detail::RouterImplDeleter::operator()(RouterImpl* impl) const noexcept {
    destroyPmrObject(impl, registrationResource());
}

void detail::RouterImpl::RouteTableDeleter::operator()(RouteTable* table) const noexcept {
    destroyPmrObject(table, resource == nullptr ? registrationResource() : resource);
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

void detail::RouteTable::setErrorHandler(HttpErrorHandler handler) noexcept {
    errorHandler_ = handler;
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
    std::pmr::vector<RouteMiddleware> frames(registrationResource());
    frames.reserve(first.size() + second.size());
    appendMaterializedMiddlewares(frames, first);
    appendMaterializedMiddlewares(frames, second);
    return frames;
}

void detail::RouterImpl::validateRouteTarget(HttpMethod method, std::string_view path) const {
    if (!RouteTable::isRoutableMethod(method)) {
        throw std::invalid_argument("route method must be routable");
    }

    for (const auto& route : pendingRoutes_) {
        if (route.method() == method && route.path() == path) {
            throw std::invalid_argument("duplicate route registration");
        }
    }
}

void detail::RouterImpl::finalize() {
    if (finalized_) {
        return;
    }

    validateNoDynamicRouteConflict(pendingRoutes_);
    routeTable_.reset(constructPmrObject<RouteTable>(registrationResource(), buildRouteTable()));
    routeTable_.get_deleter().resource = registrationResource();
    routeTable_->setErrorHandler(errorHandler_);
    finalized_ = true;
}

const detail::RouteTable& detail::RouterImpl::routeTable() const {
    if (!routeTable_) {
        throw std::logic_error("router has not been finalized");
    }
    return *routeTable_;
}

}  // namespace ruvia
