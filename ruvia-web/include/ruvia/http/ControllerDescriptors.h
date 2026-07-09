#pragma once

#include <cstddef>
#include <memory>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/app/Task.h"
#include "ruvia/http/detail/CallableRef.h"
#include "ruvia/http/detail/RegistrationResource.h"
#include "ruvia/http/HttpTypes.h"
#include "ruvia/http/MiddlewareDescriptor.h"
#include "ruvia/http/WebSocket.h"
#include "detail/HttpPmrObject.h"

namespace ruvia {

class Router;
template <typename ControllerT>
class Controller;

}  // namespace ruvia

namespace ruvia::detail {

struct ControllerStoreState;
struct ControllerStoreStateDeleter final {
    void operator()(ControllerStoreState* state) const noexcept;
};

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
    friend void runControllerRegistrars(Router& router, ControllerStore& controllerLifetimes);

    template <typename T, typename... Args>
    T& emplace(Args&&... args) {
        auto* resource = registrationResource();
        auto* raw = constructHttpPmrObject<T>(resource, std::forward<Args>(args)...);
        try {
            addLifetime(raw, &ControllerStore::destroy<T>, resource);
        } catch (...) {
            destroyHttpPmrObject(raw, resource);
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
        destroyHttpPmrObject(static_cast<T*>(target), registrationResourceOrDefault(resource));
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
    friend class ::ruvia::Controller;

    ControllerRouteBuilder(
        Router& router,
        std::string_view prefix,
        std::pmr::vector<ControllerMiddlewareDescriptor> middlewares =
            std::pmr::vector<ControllerMiddlewareDescriptor>(registrationResource()));

    void registerRoute(
        HttpMethod method,
        std::string_view path,
        ControllerRouteHandler handler,
        RequestBodyMode bodyMode,
        std::span<const ControllerMiddlewareDescriptor> middlewares = {},
        ResponseBodyMode responseMode = ResponseBodyMode::kBuffered) const;
    void registerStreamRoute(
        HttpMethod method,
        std::string_view path,
        ControllerRouteStreamHandler handler,
        ResponseBodyMode responseMode,
        std::span<const ControllerMiddlewareDescriptor> middlewares = {},
        WebSocketRouteOptions webSocketOptions = {}) const;
    [[nodiscard]] ControllerRouteBuilder createScope(
        std::string_view prefix,
        std::pmr::vector<ControllerMiddlewareDescriptor> middlewares =
            std::pmr::vector<ControllerMiddlewareDescriptor>(registrationResource())) const;

    struct OwnedPrefixTag final {};
    ControllerRouteBuilder(
        Router& router,
        std::pmr::string prefix,
        std::pmr::vector<ControllerMiddlewareDescriptor> middlewares,
        OwnedPrefixTag);

    class Impl;
    struct ImplDeleter final {
        void operator()(Impl* impl) const noexcept;
    };
    std::unique_ptr<Impl, ImplDeleter> impl_;
};

using ControllerRegistrar = void (*)(Router&, ControllerStore&);

[[nodiscard]] bool addControllerRegistrar(ControllerRegistrar registrar);
void runControllerRegistrars(Router& router, ControllerStore& controllerLifetimes);

}  // namespace ruvia::detail
