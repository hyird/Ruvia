#pragma once

#include <concepts>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include <asio/bind_executor.hpp>
#include <asio/io_context.hpp>

#include "ruvia/core/RootTask.h"
#include "ruvia/core/Task.h"
#include "ruvia/core/WorkerHandle.h"
#include "ruvia/core/detail/io/AsioAwait.h"

namespace ruvia {

namespace detail {
struct EventLoopState;
class WorkerShutdownListener;
}  // namespace detail

class EventLoopStopRegistration final {
public:
    EventLoopStopRegistration() noexcept = default;
    ~EventLoopStopRegistration() = default;

    EventLoopStopRegistration(const EventLoopStopRegistration&) = delete;
    EventLoopStopRegistration& operator=(const EventLoopStopRegistration&) = delete;
    EventLoopStopRegistration(EventLoopStopRegistration&&) noexcept = default;
    EventLoopStopRegistration& operator=(EventLoopStopRegistration&&) noexcept = default;

    [[nodiscard]] bool valid() const noexcept;
    void reset() noexcept;

private:
    explicit EventLoopStopRegistration(
        std::shared_ptr<detail::WorkerShutdownListener> listener) noexcept;

    std::shared_ptr<detail::WorkerShutdownListener> listener_;
    friend class EventLoop;
};

// A stable handle to one Ruvia runtime and its Asio execution context. EventLoop
// does not own a thread: EventLoopPool or EventLoopAttachment owns and drives
// the runtime that produced it.
class EventLoop final {
public:
    EventLoop() noexcept = default;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] bool accepting() const noexcept;
    [[nodiscard]] bool isCurrent() const noexcept;
    [[nodiscard]] WorkerId id() const noexcept;

    // A retired pool loop or attachment is invalid; accessing its context or
    // obtaining a fresh executor then throws std::logic_error. Previously
    // obtained executors must not outlive the loop's physical context storage.
    [[nodiscard]] asio::io_context& ioContext() const&;
    asio::io_context& ioContext() const&& = delete;
    [[nodiscard]] asio::io_context::executor_type executor() const;
    [[nodiscard]] WorkerHandle handle() const noexcept;

    // Reports a terminal runtime failure without throwing on the caller's
    // thread. Pool owners retain the first failure for join(); attachments
    // route it to the existing unhandled-failure diagnostic.
    void reportFailure(std::exception_ptr failure) const noexcept;

    template <typename Fn>
        requires detail::MoveOnlyFunctionTarget<void, Fn>
    [[nodiscard]] PostResult post(Fn&& fn) const {
        return dispatchHandle().post(std::forward<Fn>(fn));
    }

    // Cold-path cleanup for an already owned resource, not new business work.
    // Bypasses bounded admission but acquires retirement protection; returns
    // false once final retirement begins. Accepted callbacks and their owned
    // inputs retire before the loop can release its execution context.
    template <typename fn_type>
        requires detail::MoveOnlyFunctionTarget<void, fn_type>
    [[nodiscard]] bool defer_cleanup(fn_type&& fn) const {
        const EventLoop snapshot = *this;
        return snapshot.defer_cleanup_task(MoveOnlyFunction<void()>(std::forward<fn_type>(fn)));
    }

    // Starts one lazy Task on this loop and returns its structured completion
    // owner. The loop owns the started coroutine until completion; abandoning
    // the RootTask never destroys a suspended frame, and an abandoned failure
    // is routed to the loop failure sink.
    template <typename T>
        requires detail::AsioTaskResult<T>
    [[nodiscard]] RootTask<T> start(Task<T> task) const {
        auto retirementLease = acquireRootLease();
        if (!retirementLease) {
            throw std::runtime_error("event loop is stopping");
        }
        auto completion = std::make_shared<detail::RootTaskState<T>>(handle(), failureSink());
        const auto boundExecutor = executor();
        // drain() destroys remaining queue closures without invoking them when
        // a posted task throws. Own that destructor path so wait()/get() cannot
        // hang and an unobserved failure still reaches the loop sink.
        struct LaunchGuard final {
            std::optional<Task<T>> task_;
            std::shared_ptr<detail::RootTaskState<T>> completion_;

            LaunchGuard(Task<T> task, std::shared_ptr<detail::RootTaskState<T>> state) noexcept
                : task_(std::move(task)),
                  completion_(std::move(state)) {}
            LaunchGuard(const LaunchGuard&) = delete;
            LaunchGuard& operator=(const LaunchGuard&) = delete;
            LaunchGuard(LaunchGuard&& other) noexcept
                : task_(std::move(other.task_)),
                  completion_(std::move(other.completion_)) {}
            LaunchGuard& operator=(LaunchGuard&&) = delete;
            ~LaunchGuard() {
                // A rejected or discarded launch still owns a cold frame.
                // Retire it before waking a caller that may reclaim its inputs.
                task_.reset();
                if (completion_ != nullptr) {
                    completion_->completeFailure(
                        std::make_exception_ptr(std::runtime_error("event loop is stopping")));
                }
            }
            void release() noexcept {
                task_.reset();
                completion_.reset();
            }
        };
        struct root_completion final {
            std::shared_ptr<detail::RootTaskState<T>> completion_;
            std::shared_ptr<void> retirement_lease_;
            bool delivered_{false};

            root_completion(std::shared_ptr<detail::RootTaskState<T>> state,
                std::shared_ptr<void> lease) noexcept
                : completion_(std::move(state)),
                  retirement_lease_(std::move(lease)) {}
            root_completion(const root_completion&) = delete;
            root_completion& operator=(const root_completion&) = delete;
            root_completion(root_completion&& other) noexcept
                : completion_(std::move(other.completion_)),
                  retirement_lease_(std::move(other.retirement_lease_)),
                  delivered_(std::exchange(other.delivered_, false)) {}
            root_completion& operator=(root_completion&&) = delete;
            ~root_completion() {
                if (delivered_) {
                    completion_->publish_completion();
                }
            }
            void operator()(detail::TaskCompletionResult<T> result) noexcept {
                try {
                    if (const auto* failure = result.failure()) {
                        completion_->stage_failure(failure->exception());
                    } else if constexpr (std::is_void_v<T>) {
                        completion_->stage_value();
                    } else {
                        completion_->stage_value(std::move(*result.success()).takeValue());
                    }
                } catch (...) {
                    completion_->stage_failure(std::current_exception());
                }
                delivered_ = true;
            }
        };
        auto posted = post([completion, boundExecutor,
                               retirementLease = std::move(retirementLease),
                               launch = LaunchGuard(std::move(task), completion)]() mutable {
            try {
                detail::asyncStartTask(std::move(*launch.task_),
                    asio::bind_executor(boundExecutor, root_completion(completion, retirementLease)));
                launch.release();
            } catch (...) {
                launch.release();
                completion->completeFailure(std::current_exception());
            }
        });
        if (!posted.accepted()) {
            if (posted.status() == PostStatus::kQueueFull) {
                throw std::runtime_error("event loop root task queue is full");
            }
            throw std::runtime_error("event loop is stopping");
        }
        return RootTask<T>(std::move(completion));
    }

    // Stop cleanup is asynchronous and joined before runtime retirement. The
    // callable owner remains address-stable until its returned Task and
    // completion handler have both finished; resetting the registration only
    // unregisters cleanup that has not started.
    template <typename Fn>
        requires std::invocable<Fn&> &&
                 std::same_as<std::invoke_result_t<Fn&>, Task<void>> &&
                 std::move_constructible<std::decay_t<Fn>>
    [[nodiscard]] EventLoopStopRegistration onStop(Fn&& fn) const {
        return registerStopCallback(
            MoveOnlyFunction<Task<void>()>(std::forward<Fn>(fn)));
    }

private:
    explicit EventLoop(std::shared_ptr<detail::EventLoopState> state) noexcept;
    [[nodiscard]] const WorkerHandle& dispatchHandle() const noexcept;
    [[nodiscard]] EventLoopStopRegistration registerStopCallback(
        MoveOnlyFunction<Task<void>()> callback) const;
    [[nodiscard]] detail::EventLoopFailureSink failureSink() const;
    [[nodiscard]] std::shared_ptr<void> acquireRootLease() const;
    [[nodiscard]] bool defer_cleanup_task(MoveOnlyFunction<void()> task) const;

    std::shared_ptr<detail::EventLoopState> state_;
    friend class EventLoopPool;
    friend class EventLoopAttachment;
};

}  // namespace ruvia
