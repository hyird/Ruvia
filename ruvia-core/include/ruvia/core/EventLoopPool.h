#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <string_view>
#include <type_traits>
#include <utility>

#include <asio/io_context.hpp>
#include <asio/bind_executor.hpp>

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
                detail::asyncStartTask(
                    std::move(task),
                    asio::bind_executor(boundExecutor, [completion](detail::TaskCompletionResult<T> result) mutable {
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

struct EventLoopAttachmentOptions final {
    std::size_t mailboxCapacity{1024};
};

// Binds a worker to an io_context the caller owns and drives with run(). The
// attachment keeps the worker's endpoint valid for as long as it is alive.
// stop() and destruction are safe while another thread is inside run(): the
// external context service retains the worker state until its terminal cleanup
// handler runs, or until the context itself is destroyed. The attachment never
// calls io_context::stop() and does not join a context it does not own.
//
// If the external io_context is destroyed first, attached EventLoop handles
// become terminal; ioContext() and executor() then throw std::logic_error.
class EventLoopAttachment final {
public:
    ~EventLoopAttachment();

    EventLoopAttachment(const EventLoopAttachment&) = delete;
    EventLoopAttachment& operator=(const EventLoopAttachment&) = delete;
    // Move construction transfers one attachment without touching its context.
    // Move assignment would implicitly stop the target attachment, so it stays
    // deleted and ownership transfer remains explicit.
    EventLoopAttachment(EventLoopAttachment&& other) noexcept;
    EventLoopAttachment& operator=(EventLoopAttachment&& other) = delete;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] EventLoop loop() const noexcept;
    void stop() noexcept;

private:
    explicit EventLoopAttachment(std::shared_ptr<detail::EventLoopState> state) noexcept;

    std::shared_ptr<detail::EventLoopState> state_;
    friend EventLoopAttachment attachEventLoop(asio::io_context&, EventLoopAttachmentOptions);
};

// Attach a Ruvia worker to a caller-owned io_context. The caller drives the
// context with run() on exactly one thread and retains ownership of its
// unrelated work and stop/restart policy.
[[nodiscard]] EventLoopAttachment attachEventLoop(asio::io_context& ioContext, EventLoopAttachmentOptions options = {});

struct EventLoopPoolOptions final {
    std::size_t loopCount{0};
    std::size_t mailboxCapacity{1024};
};

class EventLoopPool final {
public:
    explicit EventLoopPool(EventLoopPoolOptions options = {});
    ~EventLoopPool();

    EventLoopPool(const EventLoopPool&) = delete;
    EventLoopPool& operator=(const EventLoopPool&) = delete;
    EventLoopPool(EventLoopPool&&) = delete;
    EventLoopPool& operator=(EventLoopPool&&) = delete;

    void start();
    void stop() noexcept;
    // Stops the pool and waits for every worker to finish. If join() happens
    // before start(), it creates short-lived owner threads to drain work accepted
    // before shutdown and to run owner-affine stop callbacks. Calling join() from
    // any worker owned by this pool throws logic_error before stopping the pool.
    void join();

    [[nodiscard]] std::size_t loopCount() const noexcept;
    [[nodiscard]] EventLoop loop(std::size_t index) const;
    [[nodiscard]] EventLoop nextLoop() noexcept;
    [[nodiscard]] EventLoop loopFor(std::uint64_t key) const noexcept;
    [[nodiscard]] EventLoop loopFor(std::string_view key) const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ruvia
