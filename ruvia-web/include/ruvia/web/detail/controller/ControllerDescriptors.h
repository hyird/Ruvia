#pragma once

// Internal startup-time controller registration contracts.

#include <cstddef>
#include <memory>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/core/Task.h"
#include "ruvia/http/HttpKnownMethod.h"
#include "ruvia/web/detail/util/CallableRef.h"
#include "ruvia/web/detail/util/RegistrationResource.h"
#include "ruvia/http/HttpResponse.h"
#include "ruvia/web/detail/middleware/MiddlewareDescriptor.h"
#include "ruvia/web/detail/router/RouteModes.h"
#include "ruvia/web/WebSocket.h"
#include "ruvia/core/memory/PmrObject.h"

namespace ruvia::detail {

class Router;

template <typename ControllerT>
class ControllerRegistrationAccess;

struct ControllerStoreState;
struct ControllerStoreStateDeleter final {
    void operator()(ControllerStoreState* state) const noexcept;
};

class ControllerStore;
using ControllerRegistrar = void (*)(Router&, ControllerStore&);

class ControllerStore final {
public:
    ControllerStore();
    ~ControllerStore();

    ControllerStore(const ControllerStore&) = delete;
    ControllerStore& operator=(const ControllerStore&) = delete;
    ControllerStore(ControllerStore&&) noexcept;
    ControllerStore& operator=(ControllerStore&&) noexcept;

private:
    template <typename ControllerT>
    friend void registerControllerInstance(Router& router, ControllerStore& controllerLifetimes);
    friend void runControllerRegistrars(Router& router, ControllerStore& controllerLifetimes, std::span<const ControllerRegistrar> registrars);

    template <typename T, typename... Args>
    T& emplace(Args&&... args) {
        auto* resource = registrationResource();
        auto* raw = constructPmrObject<T>(resource, std::forward<Args>(args)...);
        try {
            addLifetime(raw, &ControllerStore::destroy<T>, resource);
        } catch (...) {
            destroyPmrObject(raw, resource);
            throw;
        }
        return *raw;
    }

    void reserve(std::size_t count);

    [[nodiscard]] std::size_t size() const noexcept;

    using Destroy = void (*)(void*, std::pmr::memory_resource*) noexcept;

    void addLifetime(void* target, Destroy destroy, std::pmr::memory_resource* resource);

    template <typename T>
    static void destroy(void* target, std::pmr::memory_resource* resource) noexcept {
        destroyPmrObject(static_cast<T*>(target), registrationResourceOrDefault(resource));
    }

    std::unique_ptr<ControllerStoreState, ControllerStoreStateDeleter> state_;
};

using ControllerRouteHandler = CallableRef<HttpResponse, Context&>;
using ControllerRouteStreamHandler = CallableRef<void, Context&>;

class ControllerRouteBuilder final {
public:
    ControllerRouteBuilder(ControllerRouteBuilder&&) noexcept;
    ControllerRouteBuilder& operator=(ControllerRouteBuilder&&) noexcept;
    ControllerRouteBuilder(const ControllerRouteBuilder&) = delete;
    ControllerRouteBuilder& operator=(const ControllerRouteBuilder&) = delete;
    ~ControllerRouteBuilder();

private:
    template <typename ControllerT>
    friend class ControllerRegistrationAccess;

    ControllerRouteBuilder(Router& router, std::string_view prefix, std::pmr::vector<ControllerMiddlewareDescriptor> middlewares = std::pmr::vector<ControllerMiddlewareDescriptor>(registrationResource()));

    void registerRoute(HttpKnownMethod method, std::string_view path, ControllerRouteHandler handler, RequestBodyMode bodyMode, std::span<const ControllerMiddlewareDescriptor> middlewares = {}) const;
    void registerExtensionMethodRoute(std::string_view methodToken, std::string_view path, ControllerRouteHandler handler, RequestBodyMode bodyMode, std::span<const ControllerMiddlewareDescriptor> middlewares = {}) const;
    void registerResponseStreamRoute(HttpKnownMethod method, std::string_view path, ControllerRouteStreamHandler handler, std::span<const ControllerMiddlewareDescriptor> middlewares = {}) const;
    void registerSseRoute(HttpKnownMethod method, std::string_view path, ControllerRouteStreamHandler handler, std::span<const ControllerMiddlewareDescriptor> middlewares = {}) const;
    void registerWebSocketRoute(HttpKnownMethod method, std::string_view path, ControllerRouteStreamHandler handler, std::span<const ControllerMiddlewareDescriptor> middlewares = {}, WebSocketRouteConfig webSocketConfig = {}) const;
    [[nodiscard]] ControllerRouteBuilder createScope(std::string_view prefix, const std::pmr::vector<ControllerMiddlewareDescriptor>& middlewares = std::pmr::vector<ControllerMiddlewareDescriptor>(registrationResource())) const;

    struct OwnedPrefixTag final {};
    ControllerRouteBuilder(Router& router, std::pmr::string prefix, std::pmr::vector<ControllerMiddlewareDescriptor> middlewares, OwnedPrefixTag);

    class Impl;
    struct ImplDeleter final {
        void operator()(Impl* impl) const noexcept;
    };
    std::unique_ptr<Impl, ImplDeleter> impl_;
};

[[nodiscard]] bool addControllerRegistrar(ControllerRegistrar registrar);
// Static controller discovery is complete before main. The first App/TestApp
// build seals this registry; loading a controller-bearing module afterwards is
// a startup contract error instead of silently changing only later workers.
[[nodiscard]] std::pmr::vector<ControllerRegistrar> sealControllerRegistrars();
void runControllerRegistrars(Router& router, ControllerStore& controllerLifetimes, std::span<const ControllerRegistrar> registrars);

}  // namespace ruvia::detail
