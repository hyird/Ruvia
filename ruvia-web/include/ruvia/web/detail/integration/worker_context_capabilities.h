#pragma once

#include <chrono>
#include <type_traits>
#include <utility>

#include "ruvia/core/blocking_pool.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/task.h"
#include "ruvia/core/worker_handle.h"
#include "ruvia/core/worker_timer.h"
#include "ruvia/web/detail/http/context/request_bindings.h"

namespace ruvia::detail {

class worker_state_registry;

// A request and a posted job borrow the same worker-owned capabilities. No
// dispatcher or cancellation ownership is copied until an explicit offload.
class worker_context_capabilities final {
public:
    worker_context_capabilities(const worker_handle& worker_value, const ruvia::stop_token& stop_token_value,
        const worker_state_registry* worker_states) noexcept
        : worker_(worker_value),
          stop_token_(stop_token_value),
          worker_states_(worker_states) {}
    worker_context_capabilities(worker_handle&&, const ruvia::stop_token&, const worker_state_registry*) = delete;
    worker_context_capabilities(const worker_handle&, ruvia::stop_token&&, const worker_state_registry*) = delete;
    worker_context_capabilities(worker_handle&&, ruvia::stop_token&&, const worker_state_registry*) = delete;

    [[nodiscard]] const worker_handle& worker() const noexcept {
        return worker_;
    }

    [[nodiscard]] const ruvia::stop_token& stop_token() const noexcept {
        return stop_token_;
    }

    [[nodiscard]] const worker_state_registry* worker_states() const noexcept {
        return worker_states_;
    }

    template <typename state_type>
    [[nodiscard]] state_type& worker_state() const {
        return *static_cast<state_type*>(worker_state_instance(type_key<state_type>()));
    }

    // Runs on the blocking pool of this worker's event loop; without one the
    // result reports blocking_status::unavailable.
    template <typename callable_type>
    [[nodiscard]] task<blocking_result<std::invoke_result_t<callable_type&>>> run_blocking(callable_type callable) const {
        return ruvia::run_blocking(worker_, {.stop_token_ = stop_token_}, std::move(callable));
    }

    // The deadline rounds up to whole milliseconds; a non-positive one rejects
    // without submitting the callable.
    template <typename rep_type, typename period_type, typename callable_type>
    [[nodiscard]] task<blocking_result<std::invoke_result_t<callable_type&>>> run_blocking(
        std::chrono::duration<rep_type, period_type> timeout, callable_type callable) const {
        return ruvia::run_blocking(worker_,
            {.timeout_ = worker_timer_ceil_milliseconds(worker_timer_saturating_duration_cast(timeout)),
                .stop_token_ = stop_token_},
            std::move(callable));
    }

private:
    [[nodiscard]] void* worker_state_instance(const void* type_key) const;

    const worker_handle& worker_;
    const ruvia::stop_token& stop_token_;
    const worker_state_registry* worker_states_;
};

}  // namespace ruvia::detail
