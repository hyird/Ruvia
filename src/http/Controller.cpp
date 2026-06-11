#include "ruvia/http/Controller.h"

namespace ruvia::detail {
namespace {

std::pmr::vector<ControllerRegistrar>& controllerRegistrars() {
    static std::pmr::vector<ControllerRegistrar> registrars{ProcessMemory::instance().upstreamResource()};
    return registrars;
}

std::mutex& controllerRegistrarsMutex() {
    static std::mutex mutex;
    return mutex;
}

}  // namespace

bool addControllerRegistrar(ControllerRegistrar registrar) {
    std::lock_guard lock(controllerRegistrarsMutex());
    controllerRegistrars().push_back(registrar);
    return true;
}

void runControllerRegistrars(Router& router, ControllerStore& controllerLifetimes) {
    std::pmr::vector<ControllerRegistrar> registrars{ProcessMemory::instance().upstreamResource()};
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
