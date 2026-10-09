#pragma once

#include <utility>

#include "ruvia/core/move_only_function.h"
#include "ruvia/core/worker_post_types.h"

namespace ruvia {
namespace detail {
class worker_dispatcher;
}  // namespace detail

// Non-owning view of a worker_runtime_context dispatcher. The runtime must remain
// alive for accepting() and the entire synchronous post(), including callable
// construction, movement, and synchronous cleanup on rejection or abandonment.
// Do not destroy the runtime reentrantly or concurrently during post().
// close() and detach() do not end this borrow. valid() only indicates that the
// view is bound, not that the runtime is alive. Queued and rejected callables
// must independently preserve anything they borrow until execution or
// destruction.
class worker_submission_view final {
public:
    worker_submission_view() noexcept = default;

    // Indicates that this view is bound, not that the runtime is alive or attached.
    // Borrowed from worker_runtime_context; keep the runtime alive for each post().
    [[nodiscard]] bool valid() const noexcept {
        return dispatcher_ != nullptr;
    }
    [[nodiscard]] bool accepting() const noexcept;

    template <typename fn_type>
        requires detail::move_only_function_target<void, fn_type>
    [[nodiscard]] post_result_type post(fn_type&& fn) const {
        const worker_submission_view snapshot = *this;
        return snapshot.post_task(move_only_function<void()>(std::forward<fn_type>(fn)));
    }

private:
    explicit worker_submission_view(detail::worker_dispatcher* dispatcher) noexcept
        : dispatcher_(dispatcher) {}
    [[nodiscard]] post_result_type post_task(move_only_function<void()> task) const;

    detail::worker_dispatcher* dispatcher_{nullptr};
    friend class worker_runtime_context;
};

}  // namespace ruvia
