#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <type_traits>
#include <utility>

#include <asio/io_context.hpp>

#include <ruvia/core/WorkerHandle.h>

namespace ruvia {

namespace detail {
struct EventLoopState;
class WorkerShutdownListener;
}

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

class EventLoop final {
public:
    EventLoop() noexcept = default;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] bool accepting() const noexcept;
    [[nodiscard]] bool isCurrent() const noexcept;
    [[nodiscard]] WorkerId id() const noexcept;

    [[nodiscard]] asio::io_context& ioContext() const;
    [[nodiscard]] asio::io_context::executor_type executor() const;
    [[nodiscard]] WorkerHandle handle() const noexcept;

    template <typename Fn>
        requires std::invocable<std::decay_t<Fn>&>
    [[nodiscard]] PostResult post(Fn&& fn) const {
        return handle().post(std::forward<Fn>(fn));
    }

    template <typename Fn>
        requires std::invocable<std::decay_t<Fn>&>
    [[nodiscard]] EventLoopStopRegistration onStop(Fn&& fn) const {
        return registerStopCallback(
            MoveOnlyFunction<void()>(std::forward<Fn>(fn)));
    }

private:
    explicit EventLoop(std::shared_ptr<detail::EventLoopState> state) noexcept;
    [[nodiscard]] EventLoopStopRegistration registerStopCallback(
        MoveOnlyFunction<void()> callback) const;

    std::shared_ptr<detail::EventLoopState> state_;
    friend class EventLoopPool;
    friend class EventLoopAttachment;
};

struct EventLoopAttachmentOptions final {
    std::size_t mailboxCapacity{1024};
};

// Binds a worker to an io_context the caller owns and drives with run(). The
// attachment keeps the worker's endpoint valid for as long as it is alive.
//
// Teardown contract: before destroying the attachment, stop the worker (stop(),
// or let the context run dry) AND let run() return on every thread driving the
// context. Destroying the attachment while a thread is still inside run() races
// the worker's timer teardown -- this is the same ordering EventLoopPool
// guarantees internally by joining its threads before tearing a loop down.
// stop() is safe from any thread; the destructor does not (and cannot) join a
// context it does not own, so it does not wait for run() to return.
class EventLoopAttachment final {
public:
    ~EventLoopAttachment();

    EventLoopAttachment(const EventLoopAttachment&) = delete;
    EventLoopAttachment& operator=(const EventLoopAttachment&) = delete;
    EventLoopAttachment(EventLoopAttachment&& other) noexcept;
    EventLoopAttachment& operator=(EventLoopAttachment&& other) noexcept;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] EventLoop loop() const noexcept;
    void stop() noexcept;

private:
    explicit EventLoopAttachment(
        std::shared_ptr<detail::EventLoopState> state) noexcept;

    std::shared_ptr<detail::EventLoopState> state_;
    friend EventLoopAttachment attachEventLoop(
        asio::io_context&, EventLoopAttachmentOptions);
};

// Attach a Ruvia worker to a caller-owned io_context. The caller drives the
// context with run() on one or more threads. See EventLoopAttachment for the
// teardown ordering the caller must honor before destroying the returned handle.
[[nodiscard]] EventLoopAttachment attachEventLoop(
    asio::io_context& ioContext,
    EventLoopAttachmentOptions options = {});

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

}
