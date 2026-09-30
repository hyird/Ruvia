#include "ruvia/core/WorkerSubmissionView.h"

#include <stdexcept>

#include "ruvia/core/detail/worker/WorkerDispatcher.h"

namespace ruvia {

bool WorkerSubmissionView::accepting() const noexcept {
    return dispatcher_ != nullptr && dispatcher_->accepting();
}

PostResult WorkerSubmissionView::postTask(MoveOnlyFunction<void()> task) const {
    if (!task) {
        throw std::invalid_argument("worker post requires a callable task");
    }
    return dispatcher_ != nullptr
               ? dispatcher_->post(std::move(task))
               : PostResult::reject(PostStatus::kWorkerStopping, std::move(task));
}

}  // namespace ruvia
