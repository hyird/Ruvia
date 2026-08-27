#pragma once

#include "ruvia/web/detail/integration/WorkerState.h"

namespace ruvia::detail {

// Access to this worker's instance of an App::useWorkerState<T>() registration,
// shared by the contexts that can reach one: Context for a request and
// WebWorkerContext for a posted background job. Both resolve the same instance
// through the same type key, so the lookup lives here once.
//
// The deriving type supplies workerStateInstance().
template <typename Derived>
class WorkerStateCapability {
public:
    // The reference is worker-local: it stays valid for the worker's lifetime
    // but must never be handed to another worker. Throws std::logic_error for a
    // type that was not registered before App::run().
    template <typename T>
    [[nodiscard]] T& workerState() const {
        return *static_cast<T*>(
            static_cast<const Derived&>(*this).workerStateInstance(workerStateTypeKey<T>()));
    }

protected:
    constexpr WorkerStateCapability() noexcept = default;
    ~WorkerStateCapability() = default;
};

}  // namespace ruvia::detail
