#pragma once

#include <exception>
#include <memory>
#include <stdexcept>
#include <utility>

#include <asio/co_spawn.hpp>

#include "ruvia/core/AsioTask.h"
#include "ruvia/core/EventLoop.h"
#include "ruvia/core/WorkerSignal.h"

namespace ruvia::detail {

// Worker-affine completion state used by the composed client_lifecycle and
// protocol-specific retirement policies. Owns one-shot close completion and
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

    // One owner-preserving root cleanup bridge for every standalone client.
    // The factory is invoked only by the task that wins one-shot retirement.
    template <typename owner_type, typename factory_type, typename completion_type>
    void start_cleanup(std::shared_ptr<owner_type> owner, factory_type factory,
        completion_type completion) noexcept {
        if (!loop_.isCurrent()) {
            std::terminate();
        }
        if (taskStarted_ || complete_) {
            return;
        }
        taskStarted_ = true;
        try {
            asio::co_spawn(loop_.executor(), ruvia::asAwaitable(factory()),
                [owner = std::move(owner), completion = std::move(completion)](std::exception_ptr failure) mutable {
                    completion(std::move(failure));
                });
        } catch (...) {
            std::terminate();
        }
    }

    template <typename owner_type, typename start_type>
    [[nodiscard]] Task<void> shutdown_owned(std::shared_ptr<owner_type> owner,
        start_type start, ObservationMode mode) {
        if (!loop_.isCurrent()) {
            throw std::logic_error("client shutdown must run on its bound event loop");
        }
        (void)owner;  // Keep the address-stable client in the coroutine frame.
        start();
        while (!complete_) {
            co_await wait();
        }
        observeFailure(mode);
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
