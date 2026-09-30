#pragma once

#include <exception>
#include <utility>

#include "ruvia/core/EventLoop.h"
#include "ruvia/core/WorkerSignal.h"

namespace ruvia::detail {

// Worker-affine completion state shared by standalone clients. Each client
// still owns its protocol-specific phase transitions and resource teardown;
// this object owns the common one-shot close task, completion publication, and
// fatal cleanup failure reporting. Callers may observe the stored failure;
// runtime retirement only confirms completion after reporting it once.
class ClientCloseState final {
public:
    enum class ObservationMode : unsigned char {
        kCaller,
        kRetirement,
    };

    // Both capabilities are borrowed from the client's address-stable owner.
    ClientCloseState(const EventLoop& loop, const WorkerHandle& worker)
        : loop_(loop),
          signal_(worker) {}
    ClientCloseState(EventLoop&&, const WorkerHandle&) = delete;
    ClientCloseState(const EventLoop&&, const WorkerHandle&) = delete;
    ClientCloseState(const EventLoop&, WorkerHandle&&) = delete;
    ClientCloseState(const EventLoop&, const WorkerHandle&&) = delete;

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

    void finish(std::exception_ptr failure) noexcept {
        if (!taskStarted_ || complete_) {
            std::terminate();
        }
        failure_ = std::move(failure);
        const bool report = failure_ != nullptr && !failureReported_;
        failureReported_ = failureReported_ || report;
        complete_ = true;
        signal_.notify();
        if (report) {
            loop_.reportFailure(failure_);
        }
    }

    void observeFailure(ObservationMode mode) const {
        if (mode == ObservationMode::kCaller && failure_) {
            std::rethrow_exception(failure_);
        }
    }

private:
    const EventLoop& loop_;
    WorkerSignal signal_;
    bool taskStarted_{false};
    bool complete_{false};
    bool failureReported_{false};
    std::exception_ptr failure_;
};

}  // namespace ruvia::detail
