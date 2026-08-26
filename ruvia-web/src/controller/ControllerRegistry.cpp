#include "ruvia/web/detail/controller/ControllerDescriptors.h"

#include <memory_resource>
#include <mutex>
#include <stdexcept>
#include <utility>
#include "ruvia/core/memory/PmrObject.h"
#include "ruvia/core/memory/ProcessResource.h"

namespace ruvia::detail {
namespace {

struct ControllerLifetime final {
    void* target{nullptr};
    void (*destroy)(void*, std::pmr::memory_resource*) noexcept {nullptr};
    std::pmr::memory_resource* resource{nullptr};

    ControllerLifetime() noexcept = default;
    ControllerLifetime(void* targetValue,
        void (*destroyValue)(void*, std::pmr::memory_resource*) noexcept,
        std::pmr::memory_resource* resourceValue) noexcept
        : target(targetValue),
          destroy(destroyValue),
          resource(resourceValue) {}
    ControllerLifetime(const ControllerLifetime&) = delete;
    ControllerLifetime& operator=(const ControllerLifetime&) = delete;
    ControllerLifetime(ControllerLifetime&& other) noexcept
        : target(std::exchange(other.target, nullptr)),
          destroy(std::exchange(other.destroy, nullptr)),
          resource(std::exchange(other.resource, nullptr)) {}
    ControllerLifetime& operator=(ControllerLifetime&& other) noexcept {
        if (this == &other) {
            return *this;
        }
        reset();
        target = std::exchange(other.target, nullptr);
        destroy = std::exchange(other.destroy, nullptr);
        resource = std::exchange(other.resource, nullptr);
        return *this;
    }
    ~ControllerLifetime() {
        reset();
    }

    void reset() noexcept {
        if (target != nullptr && destroy != nullptr) {
            destroy(target, resource);
        }
        target = nullptr;
        destroy = nullptr;
        resource = nullptr;
    }
};

struct ControllerRegistryState final {
    std::mutex mutex;
    std::pmr::vector<ControllerRegistrar> registrars{registrationResource()};
    bool sealed{false};
};

ControllerRegistryState& controllerRegistry() {
    static ControllerRegistryState state;
    return state;
}

}  // namespace

struct ControllerStoreState final {
    std::pmr::vector<ControllerLifetime> lifetimes{registrationResource()};
};

ControllerStore::ControllerStore()
    : state_(constructPmrObject<ControllerStoreState>(registrationResource())) {}

ControllerStore::~ControllerStore() = default;

ControllerStore::ControllerStore(ControllerStore&&) noexcept = default;

ControllerStore& ControllerStore::operator=(ControllerStore&&) noexcept = default;

void ControllerStoreStateDeleter::operator()(ControllerStoreState* state) const noexcept {
    destroyPmrObject(state, registrationResource());
}

std::pmr::memory_resource* registrationResource() noexcept {
    return processResource();
}

void ControllerStore::reserve(std::size_t count) {
    state_->lifetimes.reserve(count);
}

std::size_t ControllerStore::size() const noexcept {
    return state_->lifetimes.size();
}

void ControllerStore::addLifetime(
    void* target, Destroy destroy, std::pmr::memory_resource* resource) {
    state_->lifetimes.emplace_back(target, destroy, resource);
}

bool addControllerRegistrar(ControllerRegistrar registrar) {
    if (registrar == nullptr) {
        throw std::invalid_argument("controller registrar must not be null");
    }
    auto& state = controllerRegistry();
    std::lock_guard lock(state.mutex);
    if (state.sealed) {
        throw std::logic_error(
            "controller registration is sealed; load every controller module before App::run() or "
            "TestApp::request()");
    }
    for (const auto existing : state.registrars) {
        if (existing == registrar) {
            return true;
        }
    }
    state.registrars.push_back(registrar);
    return true;
}

std::pmr::vector<ControllerRegistrar> sealControllerRegistrars() {
    std::pmr::vector<ControllerRegistrar> registrars{registrationResource()};
    auto& state = controllerRegistry();
    std::lock_guard lock(state.mutex);
    state.sealed = true;
    registrars = state.registrars;
    return registrars;
}

void runControllerRegistrars(Router& router, ControllerStore& controllerLifetimes,
    std::span<const ControllerRegistrar> registrars) {
    controllerLifetimes.reserve(controllerLifetimes.size() + registrars.size());
    for (const auto registrar : registrars) {
        registrar(router, controllerLifetimes);
    }
}

}  // namespace ruvia::detail
