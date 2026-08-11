#pragma once

#include <chrono>
#include <type_traits>
#include <utility>

#include "ruvia/core/BlockingPool.h"
#include "ruvia/core/Task.h"

namespace ruvia::detail {

// The blocking-offload surface shared by every context that owns a worker:
// Context for a request, WebWorkerContext for a posted background job. Both
// resume the caller on the same worker and answer with the same statuses, so
// the four overloads live here once instead of being copied per context.
//
// The deriving type supplies blockingPool() and blockingWorker(); everything
// user-facing is generated from those.
template <typename Derived>
class BlockingCapability {
public:
    // Runs blocking work on App::setBlockingPool()'s threads and resumes the
    // caller on its own worker with the result, so the worker keeps serving its
    // other connections meanwhile. Without this, a blocking call inside a
    // handler freezes every connection the worker owns.
    //
    // `fn` runs on a foreign thread and must own everything it touches: capture
    // by value or move, and never capture the Context, the request, its arena,
    // or any worker-owned state. The worker does not wait for a task that is
    // still running when it stops.
    //
    // Rethrows whatever `fn` threw. Throws BlockingOperationRejected when the
    // pool is saturated or stopped, which the default error path answers with
    // 503; use tryRunBlocking() to shed load yourself instead. Throws
    // std::logic_error when no pool was configured.
    template <typename Fn>
    [[nodiscard]] Task<std::invoke_result_t<Fn&>> runBlocking(Fn fn) const {
        const auto& self = static_cast<const Derived&>(*this);
        return ruvia::runBlocking(
            self.blockingPool(), self.blockingWorker(), self.blockingStopToken(), std::move(fn));
    }

    // With a deadline on the wait: a callable that has not returned within
    // `timeout` stops holding this operation, and BlockingOperationRejected is
    // thrown instead. The callable itself keeps running on its pool thread -- a
    // blocking call cannot be interrupted -- so its captured data must stay
    // self-owned exactly as above.
    template <typename Rep, typename Period, typename Fn>
    [[nodiscard]] Task<std::invoke_result_t<Fn&>> runBlocking(std::chrono::duration<Rep, Period> timeout, Fn fn) const {
        const auto& self = static_cast<const Derived&>(*this);
        return ruvia::runBlocking(
            self.blockingPool(), self.blockingWorker(), timeout, self.blockingStopToken(), std::move(fn));
    }

    // runBlocking() without the exceptions: the result carries the status, so
    // an overloaded pool can be answered with a cheaper response instead of an
    // error. Still throws std::logic_error when no pool was configured -- that
    // is a missing App::setBlockingPool(), not a runtime condition.
    template <typename Fn>
    [[nodiscard]] Task<BlockingResult<std::invoke_result_t<Fn&>>> tryRunBlocking(Fn fn) const {
        const auto& self = static_cast<const Derived&>(*this);
        return ruvia::tryRunBlocking(
            self.blockingPool(), self.blockingWorker(), self.blockingStopToken(), std::move(fn));
    }

    template <typename Rep, typename Period, typename Fn>
    [[nodiscard]] Task<BlockingResult<std::invoke_result_t<Fn&>>> tryRunBlocking(std::chrono::duration<Rep, Period> timeout, Fn fn) const {
        const auto& self = static_cast<const Derived&>(*this);
        return ruvia::tryRunBlocking(
            self.blockingPool(), self.blockingWorker(), timeout, self.blockingStopToken(), std::move(fn));
    }

protected:
    constexpr BlockingCapability() noexcept = default;
    ~BlockingCapability() = default;
};

}  // namespace ruvia::detail
