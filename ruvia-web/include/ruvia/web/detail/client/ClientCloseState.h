#pragma once

#include <exception>

#include "ruvia/core/Async.h"
#include "ruvia/core/WorkerSignal.h"

namespace ruvia::detail {

// Worker-affine completion state shared by standalone clients. Each client
// still owns its protocol-specific phase transitions and resource teardown;
// this object owns the common one-shot close task and completion publication.
class ClientCloseState final {
public:
    explicit ClientCloseState(const WorkerHandle& worker)
        : signal_(worker) {}

    [[nodiscard]] bool taskStarted() const noexcept {
        return taskStarted_;
    }

    [[nodiscard]] bool complete() const noexcept {
        return complete_;
    }

    [[nodiscard]] bool startTask() noexcept {
        if (taskStarted_ || complete_) {
            return false;
        }
        taskStarted_ = true;
        return true;
    }

    [[nodiscard]] auto wait() {
        return signal_.wait();
    }

    void notifyProgress() noexcept {
        signal_.notify();
    }

    void completeNow() noexcept {
        complete_ = true;
        signal_.notify();
    }

    // Construction failed before a client or stop callback was published.
    // No worker can observe this state yet, so no signal notification is needed.
    // A published client must complete on its worker, even if never connected.
    void completeBeforePublication() noexcept {
        if (taskStarted_ || complete_) {
            std::terminate();
        }
        complete_ = true;
    }

    void finish(const TaskCompletionResult<void>& result) {
        const auto* failed = result.failure();
        if (failed != nullptr) {
            failure_ = failed->exception();
        }
        completeNow();
        if (failed != nullptr) {
            std::rethrow_exception(failed->exception());
        }
    }

    void rethrowFailure() const {
        if (failure_) {
            std::rethrow_exception(failure_);
        }
    }

private:
    WorkerSignal signal_;
    bool taskStarted_{false};
    bool complete_{false};
    std::exception_ptr failure_;
};

}  // namespace ruvia::detail
