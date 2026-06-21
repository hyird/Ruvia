#pragma once

#include <cstddef>
#include <memory>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/app/Task.h"
#include "ruvia/http/Context.h"
#include "ruvia/http/HttpTypes.h"
#include "ruvia/memory/MemoryPool.h"
#include "ruvia/memory/PmrObject.h"
#include "ruvia/router/Router.h"

namespace ruvia::detail {

class ControllerStore final {
public:
    ControllerStore() = default;
    ControllerStore(const ControllerStore&) = delete;
    ControllerStore& operator=(const ControllerStore&) = delete;
    ControllerStore(ControllerStore&&) noexcept = default;
    ControllerStore& operator=(ControllerStore&&) noexcept = default;

    template <typename T, typename... Args>
    T& emplace(Args&&... args) {
        auto* resource = ProcessMemory::instance().upstreamResource();
        auto* raw = constructPmrObject<T>(resource, std::forward<Args>(args)...);
        try {
            lifetimes_.emplace_back(raw, &ControllerStore::destroy<T>, resource);
        } catch (...) {
            destroyPmrObject(raw, resource);
            throw;
        }
        return *raw;
    }

    void reserve(std::size_t count) {
        lifetimes_.reserve(count);
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return lifetimes_.size();
    }

private:
    struct Lifetime {
        void* target{nullptr};
        void (*destroy)(void*, std::pmr::memory_resource*) noexcept{nullptr};
        std::pmr::memory_resource* resource{nullptr};

        Lifetime() noexcept = default;
        Lifetime(
            void* targetValue,
            void (*destroyValue)(void*, std::pmr::memory_resource*) noexcept,
            std::pmr::memory_resource* resourceValue) noexcept
            : target(targetValue),
              destroy(destroyValue),
              resource(resourceValue) {}
        Lifetime(const Lifetime&) = delete;
        Lifetime& operator=(const Lifetime&) = delete;
        Lifetime(Lifetime&& other) noexcept
            : target(std::exchange(other.target, nullptr)),
              destroy(std::exchange(other.destroy, nullptr)),
              resource(std::exchange(other.resource, nullptr)) {}
        Lifetime& operator=(Lifetime&& other) noexcept {
            if (this == &other) {
                return *this;
            }
            reset();
            target = std::exchange(other.target, nullptr);
            destroy = std::exchange(other.destroy, nullptr);
            resource = std::exchange(other.resource, nullptr);
            return *this;
        }
        ~Lifetime() {
            reset();
        }

        void reset() noexcept {
            if (target != nullptr) {
                if (destroy != nullptr) {
                    destroy(target, resource);
                }
            }
            target = nullptr;
            destroy = nullptr;
            resource = nullptr;
        }
    };

    template <typename T>
    static void destroy(void* target, std::pmr::memory_resource* resource) noexcept {
        destroyPmrObject(static_cast<T*>(target), resource);
    }

    std::pmr::vector<Lifetime> lifetimes_{ProcessMemory::instance().upstreamResource()};
};

struct ControllerRouteHandler final {
    void* target{nullptr};
    Next::Invoke invoke{nullptr};

    [[nodiscard]] explicit operator bool() const noexcept {
        return invoke != nullptr;
    }

    [[nodiscard]] Task<HttpResponse> operator()(Context& context) const {
        if (invoke == nullptr) {
            throw std::logic_error("route handler is empty");
        }
        return invoke(target, context);
    }
};

struct ControllerRouteStreamHandler final {
    using Invoke = Task<void> (*)(void*, Context&);

    void* target{nullptr};
    Invoke invoke{nullptr};

    [[nodiscard]] explicit operator bool() const noexcept {
        return invoke != nullptr;
    }

    [[nodiscard]] Task<void> operator()(Context& context) const {
        if (invoke == nullptr) {
            throw std::logic_error("route stream handler is empty");
        }
        return invoke(target, context);
    }
};

struct ControllerMiddlewareDescriptor final {
    using Invoke = Task<HttpResponse> (*)(void*, Context&, const Next&);
    using Create = void* (*)();
    using Destroy = void (*)(void*) noexcept;

    Invoke invoke{nullptr};
    Create create{nullptr};
    Destroy destroy{nullptr};

    [[nodiscard]] explicit operator bool() const noexcept {
        return invoke != nullptr && create != nullptr && destroy != nullptr;
    }
};

class ControllerRouteBuilder final {
public:
    ControllerRouteBuilder(
        Router& router,
        std::string_view prefix,
        std::pmr::vector<ControllerMiddlewareDescriptor> middlewares = {});
    ControllerRouteBuilder(ControllerRouteBuilder&&) noexcept;
    ControllerRouteBuilder& operator=(ControllerRouteBuilder&&) noexcept;
    ControllerRouteBuilder(const ControllerRouteBuilder&) = delete;
    ControllerRouteBuilder& operator=(const ControllerRouteBuilder&) = delete;
    ~ControllerRouteBuilder();

    void registerRoute(
        HttpMethod method,
        std::pmr::string path,
        ControllerRouteHandler handler,
        RequestBodyMode bodyMode,
        std::pmr::vector<ControllerMiddlewareDescriptor> middlewares = {}) const;
    void registerStreamRoute(
        HttpMethod method,
        std::pmr::string path,
        ControllerRouteStreamHandler handler,
        ResponseBodyMode responseMode,
        std::pmr::vector<ControllerMiddlewareDescriptor> middlewares = {},
        WebSocketRouteOptions webSocketOptions = {}) const;
    [[nodiscard]] ControllerRouteBuilder createScope(
        std::string_view prefix,
        std::pmr::vector<ControllerMiddlewareDescriptor> middlewares = {}) const;

private:
    struct Impl;
    struct ImplDeleter final {
        std::pmr::memory_resource* resource{std::pmr::get_default_resource()};
        void operator()(Impl* impl) const noexcept;
    };
    std::unique_ptr<Impl, ImplDeleter> impl_;
};

using ControllerRegistrar = void (*)(Router&, ControllerStore&);

[[nodiscard]] bool addControllerRegistrar(ControllerRegistrar registrar);
void runControllerRegistrars(Router& router, ControllerStore& controllerLifetimes);

}  // namespace ruvia::detail
