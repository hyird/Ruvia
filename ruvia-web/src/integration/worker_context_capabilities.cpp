#include "ruvia/web/detail/integration/worker_context_capabilities.h"

#include <stdexcept>

#include "ruvia/web/detail/integration/worker_state.h"

namespace ruvia::detail {

void* worker_context_capabilities::worker_state_instance(const void* type_key) const {
    auto* instance = worker_states_ == nullptr ? nullptr : worker_states_->instance(type_key);
    if (instance == nullptr) {
        throw std::logic_error(
            "worker state type is not registered: call application::use_worker_state<state_type>() before application::run()");
    }
    return instance;
}

blocking_pool& worker_context_capabilities::require_blocking_pool() const {
    if (blocking_pool_ == nullptr) {
        throw std::logic_error("blocking pool is disabled");
    }
    return *blocking_pool_;
}

}  // namespace ruvia::detail
