#pragma once

#include <chrono>
#include <type_traits>
#include <utility>

#include "ruvia/core/BlockingPool.h"
#include "ruvia/core/StopToken.h"
#include "ruvia/core/Task.h"
#include "ruvia/core/WorkerHandle.h"
#include "ruvia/web/detail/integration/WorkerStateKey.h"

namespace ruvia::detail {

class WorkerStateRegistry;

// A request and a posted job borrow the same worker-owned capabilities. No
// dispatcher or cancellation ownership is copied until an explicit offload.
class worker_context_capabilities final {
public:
    worker_context_capabilities(const WorkerHandle& worker, const StopToken& stop_token,
        const WorkerStateRegistry* worker_states, BlockingPool* blocking_pool) noexcept
        : worker_(worker),
          stop_token_(stop_token),
          worker_states_(worker_states),
          blocking_pool_(blocking_pool) {}
    worker_context_capabilities(WorkerHandle&&, const StopToken&,
        const WorkerStateRegistry*, BlockingPool*) = delete;
    worker_context_capabilities(const WorkerHandle&, StopToken&&,
        const WorkerStateRegistry*, BlockingPool*) = delete;
    worker_context_capabilities(WorkerHandle&&, StopToken&&,
        const WorkerStateRegistry*, BlockingPool*) = delete;

    [[nodiscard]] const WorkerHandle& worker() const noexcept {
        return worker_;
    }

    [[nodiscard]] const StopToken& stop_token() const noexcept {
        return stop_token_;
    }

    [[nodiscard]] const WorkerStateRegistry* worker_states() const noexcept {
        return worker_states_;
    }

    [[nodiscard]] BlockingPool* blocking_pool() const noexcept {
        return blocking_pool_;
    }

    template <typename state_type>
    [[nodiscard]] state_type& worker_state() const {
        return *static_cast<state_type*>(worker_state_instance(workerStateTypeKey<state_type>()));
    }

    template <typename callable_type>
    [[nodiscard]] Task<std::invoke_result_t<callable_type&>> run_blocking(callable_type callable) const {
        return ruvia::runBlocking(require_blocking_pool(), worker_, stop_token_, std::move(callable));
    }

    template <typename rep_type, typename period_type, typename callable_type>
    [[nodiscard]] Task<std::invoke_result_t<callable_type&>> run_blocking(
        std::chrono::duration<rep_type, period_type> timeout, callable_type callable) const {
        return ruvia::runBlocking(require_blocking_pool(), worker_, timeout, stop_token_, std::move(callable));
    }

    template <typename callable_type>
    [[nodiscard]] Task<BlockingResult<std::invoke_result_t<callable_type&>>> try_run_blocking(callable_type callable) const {
        return ruvia::tryRunBlocking(require_blocking_pool(), worker_, stop_token_, std::move(callable));
    }

    template <typename rep_type, typename period_type, typename callable_type>
    [[nodiscard]] Task<BlockingResult<std::invoke_result_t<callable_type&>>> try_run_blocking(
        std::chrono::duration<rep_type, period_type> timeout, callable_type callable) const {
        return ruvia::tryRunBlocking(require_blocking_pool(), worker_, timeout, stop_token_, std::move(callable));
    }

private:
    [[nodiscard]] void* worker_state_instance(const void* type_key) const;
    [[nodiscard]] BlockingPool& require_blocking_pool() const;

    const WorkerHandle& worker_;
    const StopToken& stop_token_;
    const WorkerStateRegistry* worker_states_;
    BlockingPool* blocking_pool_;
};

}  // namespace ruvia::detail
