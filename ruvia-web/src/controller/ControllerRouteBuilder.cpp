#include "ruvia/web/detail/router/RouterImpl.h"

#include "ruvia/web/detail/util/RegistrationResource.h"

// The route-registration surface a controller sees. Every call forwards to the
// RouterImpl it was scoped from, carrying the prefix and the middlewares that
// scope added; the router's own side of the same registration is in
// router/RouterRegistration.cpp.

namespace ruvia {

using namespace detail;

namespace {

std::pmr::string joinControllerPaths(std::string_view prefix, std::string_view path) {
    auto* resource = registrationResource();
    if (prefix.empty() || prefix == "/") {
        if (path.empty()) {
            return std::pmr::string{"/", resource};
        }
        if (path.front() == '/') {
            return std::pmr::string(path, resource);
        }

        std::pmr::string output(resource);
        output.reserve(path.size() + 1);
        output.push_back('/');
        output.append(path);
        return output;
    }

    std::pmr::string output(resource);
    output.reserve(prefix.size() + path.size() + 1);
    output.append(prefix.front() == '/' ? prefix : std::string_view{});
    if (prefix.front() != '/') {
        output.push_back('/');
        output.append(prefix);
    }
    if (output.size() > 1 && output.back() == '/') {
        output.pop_back();
    }
    if (path.empty() || path == "/") {
        return output;
    }
    if (path.front() == '/') {
        path.remove_prefix(1);
    }
    output.push_back('/');
    output.append(path);
    return output;
}

template <typename BaseRange, typename ExtraRange>
[[nodiscard]] std::pmr::vector<ControllerMiddlewareDescriptor> mergeMiddlewareDescriptors(
    const BaseRange& base,
    const ExtraRange& extra) {
    std::pmr::vector<ControllerMiddlewareDescriptor> middlewares(registrationResource());
    middlewares.reserve(base.size() + extra.size());
    middlewares.insert(middlewares.end(), base.begin(), base.end());
    middlewares.insert(middlewares.end(), extra.begin(), extra.end());
    return middlewares;
}

[[nodiscard]] std::pmr::vector<ControllerMiddlewareDescriptor> normalizeControllerMiddlewares(
    std::pmr::vector<ControllerMiddlewareDescriptor> middlewares) {
    if (middlewares.get_allocator().resource() == registrationResource()) {
        return middlewares;
    }

    std::pmr::vector<ControllerMiddlewareDescriptor> normalized(registrationResource());
    normalized.insert(normalized.end(), middlewares.begin(), middlewares.end());
    return normalized;
}

}  // namespace

void detail::ControllerRouteBuilder::ImplDeleter::operator()(Impl* impl) const noexcept {
    destroyPmrObject(impl, registrationResource());
}

detail::ControllerRouteBuilder::ControllerRouteBuilder(
    Router& router,
    std::string_view prefix,
    std::pmr::vector<ControllerMiddlewareDescriptor> middlewares)
    : ControllerRouteBuilder(
          router,
          joinControllerPaths({}, prefix),
          std::move(middlewares),
          OwnedPrefixTag{}) {}

detail::ControllerRouteBuilder::ControllerRouteBuilder(
    Router& router,
    std::pmr::string prefix,
    std::pmr::vector<ControllerMiddlewareDescriptor> middlewares,
    OwnedPrefixTag)
    : impl_(
          constructPmrObject<Impl>(
              registrationResource(),
              router,
              std::move(prefix),
              normalizeControllerMiddlewares(std::move(middlewares)))) {}

detail::ControllerRouteBuilder::ControllerRouteBuilder(ControllerRouteBuilder&&) noexcept = default;

detail::ControllerRouteBuilder& detail::ControllerRouteBuilder::operator=(ControllerRouteBuilder&&) noexcept = default;

detail::ControllerRouteBuilder::~ControllerRouteBuilder() = default;

void detail::ControllerRouteBuilder::registerRoute(
    HttpKnownMethod method,
    std::string_view path,
    ControllerRouteHandler handler,
    RequestBodyMode bodyMode,
    std::span<const ControllerMiddlewareDescriptor> middlewares) const {
    RouterImpl::from(impl_->router()).registerRoute(
        method,
        joinControllerPaths(impl_->prefix(), path),
        std::move(handler),
        bodyMode,
        impl_->middlewares(),
        middlewares);
}

void detail::ControllerRouteBuilder::registerResponseStreamRoute(
    HttpKnownMethod method,
    std::string_view path,
    ControllerRouteStreamHandler handler,
    std::span<const ControllerMiddlewareDescriptor> middlewares) const {
    RouterImpl::from(impl_->router()).registerResponseStreamRoute(
        method,
        joinControllerPaths(impl_->prefix(), path),
        std::move(handler),
        impl_->middlewares(),
        middlewares);
}

void detail::ControllerRouteBuilder::registerSseRoute(
    HttpKnownMethod method,
    std::string_view path,
    ControllerRouteStreamHandler handler,
    std::span<const ControllerMiddlewareDescriptor> middlewares) const {
    RouterImpl::from(impl_->router()).registerSseRoute(
        method,
        joinControllerPaths(impl_->prefix(), path),
        std::move(handler),
        impl_->middlewares(),
        middlewares);
}

void detail::ControllerRouteBuilder::registerWebSocketRoute(
    HttpKnownMethod method,
    std::string_view path,
    ControllerRouteStreamHandler handler,
    std::span<const ControllerMiddlewareDescriptor> middlewares,
    WebSocketRouteOptions webSocketOptions) const {
    RouterImpl::from(impl_->router()).registerWebSocketRoute(
        method,
        joinControllerPaths(impl_->prefix(), path),
        std::move(handler),
        impl_->middlewares(),
        middlewares,
        webSocketOptions);
}

detail::ControllerRouteBuilder detail::ControllerRouteBuilder::createScope(
    std::string_view prefix,
    std::pmr::vector<ControllerMiddlewareDescriptor> middlewares) const {
    auto merged = mergeMiddlewareDescriptors(impl_->middlewares(), middlewares);
    return ControllerRouteBuilder(
        impl_->router(),
        joinControllerPaths(impl_->prefix(), prefix),
        std::move(merged),
        OwnedPrefixTag{});
}

}  // namespace ruvia
