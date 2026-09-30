#pragma once

#include <utility>

#include "ruvia/core/MoveOnlyFunction.h"
#include "ruvia/core/WorkerPostTypes.h"

namespace ruvia {
namespace detail {
class WorkerDispatcher;
}  // namespace detail

// Non-owning view of a WorkerRuntimeContext dispatcher. The runtime must remain
// alive for accepting() and the entire synchronous post(), including callable
// construction, movement, and synchronous cleanup on rejection or abandonment.
// Do not destroy the runtime reentrantly or concurrently during post().
// close() and detach() do not end this borrow. valid() only indicates that the
// view is bound, not that the runtime is alive. Queued and rejected callables
// must independently preserve anything they borrow until execution or
// destruction.
class WorkerSubmissionView final {
public:
    WorkerSubmissionView() noexcept = default;

    // Indicates that this view is bound, not that the runtime is alive or attached.
    // Borrowed from WorkerRuntimeContext; keep the runtime alive for each post().
    [[nodiscard]] bool valid() const noexcept {
        return dispatcher_ != nullptr;
    }
    [[nodiscard]] bool accepting() const noexcept;

    template <typename Fn>
        requires detail::MoveOnlyFunctionTarget<void, Fn>
    [[nodiscard]] PostResult post(Fn&& fn) const {
        const WorkerSubmissionView snapshot = *this;
        return snapshot.postTask(MoveOnlyFunction<void()>(std::forward<Fn>(fn)));
    }

private:
    explicit WorkerSubmissionView(detail::WorkerDispatcher* dispatcher) noexcept
        : dispatcher_(dispatcher) {}
    [[nodiscard]] PostResult postTask(MoveOnlyFunction<void()> task) const;

    detail::WorkerDispatcher* dispatcher_{nullptr};
    friend class WorkerRuntimeContext;
};

}  // namespace ruvia
