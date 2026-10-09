#include <memory_resource>
#include <mutex>
#include <stdexcept>
#include <utility>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/memory/process_resource.h"
#include "ruvia/web/detail/controller/controller_descriptors.h"

namespace ruvia::detail {
namespace {

struct controller_lifetime final {
    void* target_{nullptr};
    void (*destroy_)(void*, std::pmr::memory_resource*) noexcept {nullptr};
    std::pmr::memory_resource* resource_{nullptr};

    controller_lifetime() noexcept = default;
    controller_lifetime(void* target_value,
        void (*destroy_value)(void*, std::pmr::memory_resource*) noexcept,
        std::pmr::memory_resource* resource_value) noexcept
        : target_(target_value),
          destroy_(destroy_value),
          resource_(resource_value) {}
    controller_lifetime(const controller_lifetime&) = delete;
    controller_lifetime& operator=(const controller_lifetime&) = delete;
    controller_lifetime(controller_lifetime&& other) noexcept
        : target_(std::exchange(other.target_, nullptr)),
          destroy_(std::exchange(other.destroy_, nullptr)),
          resource_(std::exchange(other.resource_, nullptr)) {}
    controller_lifetime& operator=(controller_lifetime&& other) noexcept {
        if (this == &other) {
            return *this;
        }
        reset();
        target_ = std::exchange(other.target_, nullptr);
        destroy_ = std::exchange(other.destroy_, nullptr);
        resource_ = std::exchange(other.resource_, nullptr);
        return *this;
    }
    ~controller_lifetime() {
        reset();
    }

    void reset() noexcept {
        if (target_ != nullptr && destroy_ != nullptr) {
            destroy_(target_, resource_);
        }
        target_ = nullptr;
        destroy_ = nullptr;
        resource_ = nullptr;
    }
};

struct controller_registry_state final {
    std::mutex mutex_;
    std::pmr::vector<controller_registrar_type> registrars_{registration_resource()};
    bool sealed_{false};
};

controller_registry_state& controller_registry() {
    static controller_registry_state state;
    return state;
}

}  // namespace

struct controller_store_state final {
    std::pmr::vector<controller_lifetime> lifetimes_{registration_resource()};
};

controller_store::controller_store()
    : state_(construct_pmr_object<controller_store_state>(registration_resource())) {}

controller_store::~controller_store() = default;

controller_store::controller_store(controller_store&&) noexcept = default;

controller_store& controller_store::operator=(controller_store&&) noexcept = default;

void controller_store_state_deleter::operator()(controller_store_state* state_value) const noexcept {
    destroy_pmr_object(state_value, registration_resource());
}

std::pmr::memory_resource* registration_resource() noexcept {
    return process_resource();
}

void controller_store::reserve(std::size_t count) {
    state_->lifetimes_.reserve(count);
}

std::size_t controller_store::size() const noexcept {
    return state_->lifetimes_.size();
}

void controller_store::add_lifetime(
    void* target, destroy_type destroy, std::pmr::memory_resource* resource) {
    state_->lifetimes_.emplace_back(target, destroy, resource);
}

bool add_controller_registrar(controller_registrar_type registrar_value) {
    if (registrar_value == nullptr) {
        throw std::invalid_argument("controller registrar must not be null");
    }
    auto& state_value = controller_registry();
    std::lock_guard lock(state_value.mutex_);
    if (state_value.sealed_) {
        throw std::logic_error(
            "controller registration is sealed; load every controller module before application::run() or "
            "test_app::request()");
    }
    for (const auto existing : state_value.registrars_) {
        if (existing == registrar_value) {
            return true;
        }
    }
    state_value.registrars_.push_back(registrar_value);
    return true;
}

std::pmr::vector<controller_registrar_type> seal_controller_registrars() {
    std::pmr::vector<controller_registrar_type> registrars{registration_resource()};
    auto& state_value = controller_registry();
    std::lock_guard lock(state_value.mutex_);
    state_value.sealed_ = true;
    registrars = state_value.registrars_;
    return registrars;
}

void run_controller_registrars(router& router_value, controller_store& controller_lifetimes,
    std::span<const controller_registrar_type> registrars) {
    controller_lifetimes.reserve(controller_lifetimes.size() + registrars.size());
    for (const auto registrar : registrars) {
        registrar(router_value, controller_lifetimes);
    }
}

}  // namespace ruvia::detail
