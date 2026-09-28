#pragma once

#include <chrono>
#include <coroutine>
#include <cstdint>
#include <memory>

#include "ruvia/core/memory/PmrObject.h"

namespace ruvia {

class EventLoop;
class WorkerRuntimeContext;

struct WorkerNotificationOptions final {
    // Bounds the Windows native wake-pipe connection wait (at most one minute).
    // Synchronous setup system calls are not hard-real-time bounded.
    std::chrono::milliseconds startupTimeout{std::chrono::seconds(5)};
};

// kCoalesced means an existing pending notification covers this call;
// notifications are latched, not counted.
enum class WorkerNotificationStatus : std::uint8_t {
    kNotified,
    kCoalesced,
    kClosed,
};

enum class WorkerNotificationWaitStatus : std::uint8_t {
    kNotified,
    kClosed,
};

namespace detail {
class WorkerNotificationState;
}  // namespace detail

// A startup-created, address-stable, cross-thread latched wake channel for one
// worker runtime. The native wake pair is created at construction; the receiver
// Asio handle is adopted on the worker's first wait. The EventLoop constructor
// retains its loop, while the WorkerRuntimeContext constructor borrows that
// runtime, which must outlive the notification. Before destruction, every
// notify() call must have returned and no future call may begin. close()
// rejects new notifications but does not join producers. The owner worker must
// explicitly close the notification and join every suspended wait before its
// runtime stops. A cold, never-awaited instance can be destroyed directly.
class WorkerNotification final {
public:
    class WaitAwaiter final {
    public:
        WaitAwaiter(const WaitAwaiter&) = delete;
        WaitAwaiter& operator=(const WaitAwaiter&) = delete;

        [[nodiscard]] bool await_ready();
        [[nodiscard]] bool await_suspend(std::coroutine_handle<> continuation);
        [[nodiscard]] WorkerNotificationWaitStatus await_resume();

    private:
        explicit WaitAwaiter(WorkerNotification& owner) noexcept;

        WorkerNotification* owner_;
        friend class WorkerNotification;
    };

    explicit WorkerNotification(EventLoop loop, WorkerNotificationOptions options = {});
    explicit WorkerNotification(
        WorkerRuntimeContext& runtime, WorkerNotificationOptions options = {});
    ~WorkerNotification();

    WorkerNotification(const WorkerNotification&) = delete;
    WorkerNotification& operator=(const WorkerNotification&) = delete;
    WorkerNotification(WorkerNotification&&) = delete;
    WorkerNotification& operator=(WorkerNotification&&) = delete;

    // Safe from any thread. The method updates its atomic latch and performs a
    // nonblocking native write; an existing latch or proven full channel coalesces.
    [[nodiscard]] WorkerNotificationStatus notify() noexcept;

    // Returns the one stable awaiter; waiting is worker-affine and permits one
    // outstanding wait. Constructing or discarding a cold Task around wait()
    // does not register an operation.
    [[nodiscard]] WaitAwaiter& wait() noexcept;

    // Worker-affine and nonblocking. A pending wait completes as kClosed after
    // Asio delivers and drains its cancellation completion.
    void close();

private:
    std::unique_ptr<detail::WorkerNotificationState,
        detail::PmrObjectDeleter<detail::WorkerNotificationState>>
        state_;
    WaitAwaiter waitAwaiter_;
};

}  // namespace ruvia
