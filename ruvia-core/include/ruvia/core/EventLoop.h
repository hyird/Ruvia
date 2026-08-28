#pragma once

#include <concepts>
#include <cstdint>
#include <exception>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include <asio/bind_executor.hpp>
#include <asio/io_context.hpp>

#include <ruvia/core/RootTask.h>
#include <ruvia/core/Task.h>
#include <ruvia/core/WorkerHandle.h>
#include <ruvia/core/detail/io/AsioAwait.h>

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
    explicit EventLoopStopRegistration(std::shared_ptr<detail::WorkerShutdownListener> listener) noexcept;

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

    [[nodiscard]] asio::io_context& ioContext() const&;
    asio::io_context& ioContext() const&& = delete;
    [[nodiscard]] asio::io_context::executor_type executor() const;
    [[nodiscard]] WorkerHandle handle() const noexcept;

    template <typename Fn>
        requires detail::MoveOnlyFunctionTarget<void, Fn>
    [[nodiscard]] PostResult post(Fn&& fn) const {
        return handle().post(std::forward<Fn>(fn));
    }

    // Starts one lazy Task on this loop and returns its structured completion
    // owner. The loop owns the started coroutine until completion; abandoning
    // the RootTask never destroys a suspended frame, and an abandoned failure
    // is routed to the loop failure sink.
    template <typename T>
        requires detail::AsioTaskResult<T>
    [[nodiscard]] RootTask<T> start(Task<T> task) const {
        auto completion = std::make_shared<detail::RootTaskState<T>>(handle(), failureSink());
        const auto boundExecutor = executor();
        auto posted = post([task = std::move(task), completion, boundExecutor]() mutable {
            try {
                detail::asyncStartTask(std::move(task), asio::bind_executor(boundExecutor, [completion](detail::TaskCompletionResult<T> result) mutable {
                    if (const auto* failure = result.failure()) {
                        completion->completeFailure(failure->exception());
                        return;
                    }
                    if constexpr (std::is_void_v<T>) {
                        completion->completeValue();
                    } else {
                        completion->completeValue(std::move(*result.success()).takeValue());
                    }
                }));
            } catch (...) {
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

    template <typename Fn>
        requires detail::MoveOnlyFunctionTarget<void, Fn>
    [[nodiscard]] EventLoopStopRegistration onStop(Fn&& fn) const {
        return registerStopCallback(MoveOnlyFunction<void()>(std::forward<Fn>(fn)));
    }

private:
    explicit EventLoop(std::shared_ptr<detail::EventLoopState> state) noexcept;
    [[nodiscard]] EventLoopStopRegistration registerStopCallback(MoveOnlyFunction<void()> callback) const;
    [[nodiscard]] detail::EventLoopFailureSink failureSink() const;

    std::shared_ptr<detail::EventLoopState> state_;
    friend class EventLoopPool;
    friend class EventLoopAttachment;
};

}  // namespace ruvia
