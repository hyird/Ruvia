#include <memory_resource>
#include <mutex>
#include <stdexcept>
#include <utility>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/web/detail/controller/controller_descriptors.h"

namespace ruvia::detail {
namespace {

struct controller_registry_state final {
    std::mutex mutex_;
    std::pmr::vector<controller_registrar> registrars_{registration_resource()};
    bool sealed_{false};
};

controller_registry_state& controller_registry() {
    static controller_registry_state state;
    return state;
}

}  // namespace

controller_store::controller_store() = default;

controller_store::controller_store(std::span<const controller_registrar> registrars) {
    lifetimes_.reserve(registrars.size());
    for (const auto& registrar : registrars) {
        registrar.create_(*this);
    }
}

controller_store::~controller_store() = default;

controller_store::controller_store(controller_store&&) noexcept = default;

controller_store& controller_store::operator=(controller_store&&) noexcept = default;

std::pmr::memory_resource* registration_resource() noexcept {
    return process_resource();
}

std::size_t controller_store::size() const noexcept {
    return lifetimes_.size();
}

void* controller_store::instance(std::size_t slot) const noexcept {
    return lifetimes_[slot].target_;
}

void controller_store::add_lifetime(void* target, destroy_type destroy) {
    lifetimes_.emplace_back(target, destroy);
}

bool add_controller_registrar(controller_registrar registrar_value) {
    auto& state_value = controller_registry();
    std::lock_guard lock(state_value.mutex_);
    if (state_value.sealed_) {
        throw std::logic_error(
            "controller registration is sealed; load every controller module before application::run() or "
            "test_app::request()");
    }
    for (const auto& existing : state_value.registrars_) {
        if (existing == registrar_value) {
            return true;
        }
    }
    state_value.registrars_.push_back(registrar_value);
    return true;
}

std::pmr::vector<controller_registrar> seal_controller_registrars() {
    std::pmr::vector<controller_registrar> registrars{registration_resource()};
    auto& state_value = controller_registry();
    std::lock_guard lock(state_value.mutex_);
    state_value.sealed_ = true;
    registrars = state_value.registrars_;
    return registrars;
}

}  // namespace ruvia::detail
