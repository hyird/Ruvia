#include "ruvia/web/detail/router/RouterInternal.h"

#include "ruvia/web/detail/RegistrationResource.h"
#include "ruvia/core/memory/PmrObject.h"

#include <stdexcept>

namespace ruvia {

using namespace detail;

namespace {

Task<void> ignoreExpiredNext(Next::State) {
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

}  // namespace

Next::Awaitable Next::operator()() & {
    auto state = state_;
    auto* control = state.control;
    if (control == nullptr || !control->active) {
        state.repeated = true;
        return Awaitable(state, &ignoreExpiredNext);
    }

    state.repeated = control->invoked;
    if (!control->invoked) {
        control->invoked = true;
    }
    return Awaitable(state, invoke_);
}

detail::RouterImpl::RouterImpl(Router& router) noexcept
    : owner(router),
      resource_(registrationResource()),
      pendingRoutes_(resource_),
      middlewareLifetimes_(resource_),
      routeTable_(nullptr, RouteTableDeleter{resource_}) {}

void detail::RouterImplDeleter::operator()(RouterImpl* impl) const noexcept {
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

void detail::RouterImpl::finalize() {
    if (finalized_) {
        return;
    }

    validateNoDynamicRouteConflict(pendingRoutes_);
    routeTable_.reset(constructPmrObject<RouteTable>(resource_, buildRouteTable()));
    routeTable_.get_deleter().resource = resource_;
    routeTable_->setErrorHandler(errorHandler_);
    routeTable_->setNotFoundHandler(notFoundHandler_);
    finalized_ = true;
}

const detail::RouteTable& detail::RouterImpl::routeTable() const {
    if (!routeTable_) {
        throw std::logic_error("router has not been finalized");
    }
    return *routeTable_;
}

}  // namespace ruvia
