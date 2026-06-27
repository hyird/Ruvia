#include "ruvia/http/ControllerDescriptors.h"

#include <memory_resource>
#include <mutex>
#include <utility>

#include "ruvia/memory/MemoryPool.h"
#include "ruvia/memory/PmrObject.h"

namespace ruvia::detail {
namespace {

struct ControllerLifetime final {
    void* target{nullptr};
    void (*destroy)(void*, std::pmr::memory_resource*) noexcept{nullptr};
    std::pmr::memory_resource* resource{nullptr};

    ControllerLifetime() noexcept = default;
    ControllerLifetime(
        void* targetValue,
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

}  // namespace

struct ControllerStoreState final {
    std::pmr::vector<ControllerLifetime> lifetimes{registrationResource()};
};

namespace {

std::pmr::vector<ControllerRegistrar>& controllerRegistrars() {
    static std::pmr::vector<ControllerRegistrar> registrars{registrationResource()};
    return registrars;
}

std::mutex& controllerRegistrarsMutex() {
    static std::mutex mutex;
    return mutex;
}

}  // namespace

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
    void* target,
    Destroy destroy,
    std::pmr::memory_resource* resource) {
    state_->lifetimes.emplace_back(target, destroy, resource);
}

bool addControllerRegistrar(ControllerRegistrar registrar) {
    std::lock_guard lock(controllerRegistrarsMutex());
    controllerRegistrars().push_back(registrar);
    return true;
}

void runControllerRegistrars(Router& router, ControllerStore& controllerLifetimes) {
    std::pmr::vector<ControllerRegistrar> registrars{registrationResource()};
    {
        std::lock_guard lock(controllerRegistrarsMutex());
        registrars = controllerRegistrars();
    }

    controllerLifetimes.reserve(controllerLifetimes.size() + registrars.size());
    for (const auto registrar : registrars) {
        registrar(router, controllerLifetimes);
    }
}

}  // namespace ruvia::detail
