#include "ruvia/core/worker_submission_view.h"

#include <stdexcept>

#include "ruvia/core/detail/worker/worker_dispatcher.h"

namespace ruvia {

bool worker_submission_view::accepting() const noexcept {
    return dispatcher_ != nullptr && dispatcher_->accepting();
}

post_result_type worker_submission_view::post_task(move_only_function<void()> task_value) const {
    if (!task_value) {
        throw std::invalid_argument("worker post requires a callable task");
    }
    return dispatcher_ != nullptr
               ? dispatcher_->post(std::move(task_value))
               : post_result_type::reject(post_status::worker_stopping, std::move(task_value));
}

}  // namespace ruvia
