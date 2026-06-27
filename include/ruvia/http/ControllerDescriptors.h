#pragma once

#include <cstddef>
#include <memory>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/app/Task.h"
#include "ruvia/http/detail/CallableRef.h"
#include "ruvia/http/HttpTypes.h"
#include "ruvia/http/MiddlewareDescriptor.h"
#include "ruvia/http/WebSocket.h"

namespace ruvia {

class Router;

}  // namespace ruvia

namespace ruvia::detail {

struct ControllerStoreState;
struct ControllerStoreStateDeleter final {
    void operator()(ControllerStoreState* state) const noexcept;
};
[[nodiscard]] std::pmr::memory_resource* controllerStoreResource() noexcept;

class ControllerStore final {
public:
    ControllerStore();
    ~ControllerStore();

    ControllerStore(const ControllerStore&) = delete;
    ControllerStore& operator=(const ControllerStore&) = delete;
    ControllerStore(ControllerStore&&) noexcept;
    ControllerStore& operator=(ControllerStore&&) noexcept;

    template <typename T, typename... Args>
    T& emplace(Args&&... args) {
        auto* resource = controllerStoreResource();
        auto* storage = resource->allocate(sizeof(T), alignof(T));
        auto* raw = static_cast<T*>(storage);
        try {
            std::construct_at(raw, std::forward<Args>(args)...);
        } catch (...) {
            resource->deallocate(storage, sizeof(T), alignof(T));
            throw;
        }
        try {
            addLifetime(raw, &ControllerStore::destroy<T>, resource);
        } catch (...) {
            std::destroy_at(raw);
            resource->deallocate(storage, sizeof(T), alignof(T));
            throw;
        }
        return *raw;
    }

    void reserve(std::size_t count);

    [[nodiscard]] std::size_t size() const noexcept;

private:
    using Destroy = void (*)(void*, std::pmr::memory_resource*) noexcept;

    void addLifetime(void* target, Destroy destroy, std::pmr::memory_resource* resource);

    template <typename T>
    static void destroy(void* target, std::pmr::memory_resource* resource) noexcept {
        if (target == nullptr) {
            return;
        }
        auto* value = static_cast<T*>(target);
        std::destroy_at(value);
        resource->deallocate(value, sizeof(T), alignof(T));
    }

    std::unique_ptr<ControllerStoreState, ControllerStoreStateDeleter> state_;
};

using ControllerRouteHandler = CallableRef<HttpResponse, Context&>;
using ControllerRouteStreamHandler = CallableRef<void, Context&>;

class ControllerRouteBuilder final {
public:
    ControllerRouteBuilder(
        Router& router,
        std::string_view prefix,
        std::pmr::vector<ControllerMiddlewareDescriptor> middlewares =
            std::pmr::vector<ControllerMiddlewareDescriptor>(controllerStoreResource()));
    ControllerRouteBuilder(ControllerRouteBuilder&&) noexcept;
    ControllerRouteBuilder& operator=(ControllerRouteBuilder&&) noexcept;
    ControllerRouteBuilder(const ControllerRouteBuilder&) = delete;
    ControllerRouteBuilder& operator=(const ControllerRouteBuilder&) = delete;
    ~ControllerRouteBuilder();

    void registerRoute(
        HttpMethod method,
        std::string_view path,
        ControllerRouteHandler handler,
        RequestBodyMode bodyMode,
        std::pmr::vector<ControllerMiddlewareDescriptor> middlewares =
            std::pmr::vector<ControllerMiddlewareDescriptor>(controllerStoreResource()),
        ResponseBodyMode responseMode = ResponseBodyMode::kBuffered) const;
    void registerStreamRoute(
        HttpMethod method,
        std::string_view path,
        ControllerRouteStreamHandler handler,
        ResponseBodyMode responseMode,
        std::pmr::vector<ControllerMiddlewareDescriptor> middlewares =
            std::pmr::vector<ControllerMiddlewareDescriptor>(controllerStoreResource()),
        WebSocketRouteOptions webSocketOptions = {}) const;
    [[nodiscard]] ControllerRouteBuilder createScope(
        std::string_view prefix,
        std::pmr::vector<ControllerMiddlewareDescriptor> middlewares =
            std::pmr::vector<ControllerMiddlewareDescriptor>(controllerStoreResource())) const;

private:
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
