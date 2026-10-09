#pragma once

#include <chrono>
#include <coroutine>
#include <cstdint>
#include <memory>

#include "ruvia/core/memory/pmr_object.h"

namespace ruvia {

class event_loop;
class worker_runtime_context;

struct worker_notification_options final {
    // Bounds the Windows native wake-pipe connection wait (at most one minute).
    // Synchronous setup system calls are not hard-real-time bounded.
    std::chrono::milliseconds startup_timeout_{std::chrono::seconds(5)};
};

// coalesced means an existing pending notification covers this call;
// notifications are latched, not counted.
enum class worker_notification_status : std::uint8_t {
    notified,
    coalesced,
    closed,
};

enum class worker_notification_wait_status : std::uint8_t {
    notified,
    closed,
};

namespace detail {
class worker_notification_state;
}  // namespace detail

// A startup-created, address-stable, cross-thread latched wake channel for one
// worker runtime. The native wake pair is created at construction; the receiver
// Asio handle is adopted on the worker's first wait. The event_loop constructor
// retains its loop, while the worker_runtime_context constructor borrows that
// runtime, which must outlive the notification. Before destruction, every
// notify() call must have returned and no future call may begin. close()
// rejects new notifications but does not join producers. The owner worker must
// explicitly close the notification and join every suspended wait before its
// runtime stops. A cold, never-awaited instance can be destroyed directly.
class worker_notification final {
public:
    class wait_awaiter_type final {
    public:
        wait_awaiter_type(const wait_awaiter_type&) = delete;
        wait_awaiter_type& operator=(const wait_awaiter_type&) = delete;

        [[nodiscard]] bool await_ready();
        [[nodiscard]] bool await_suspend(std::coroutine_handle<> continuation);
        [[nodiscard]] worker_notification_wait_status await_resume();

    private:
        explicit wait_awaiter_type(worker_notification& owner_value) noexcept;

        worker_notification* owner_;
        friend class worker_notification;
    };

    explicit worker_notification(event_loop loop, worker_notification_options options = {});
    explicit worker_notification(
        worker_runtime_context& runtime, worker_notification_options options = {});
    ~worker_notification();

    worker_notification(const worker_notification&) = delete;
    worker_notification& operator=(const worker_notification&) = delete;
    worker_notification(worker_notification&&) = delete;
    worker_notification& operator=(worker_notification&&) = delete;

    // Safe from any thread. The method updates its atomic latch and performs a
    // nonblocking native write; an existing latch or proven full channel coalesces.
    [[nodiscard]] worker_notification_status notify() noexcept;

    // Returns the one stable awaiter; waiting is worker-affine and permits one
    // outstanding wait. Constructing or discarding a cold task around wait()
    // does not register an operation.
    [[nodiscard]] wait_awaiter_type& wait() noexcept;

    // Worker-affine and nonblocking. A pending wait completes as closed after
    // Asio delivers and drains its cancellation completion.
    void close();

private:
    std::unique_ptr<detail::worker_notification_state,
        detail::pmr_object_deleter<detail::worker_notification_state>>
        state_;
    wait_awaiter_type wait_awaiter_;
};

}  // namespace ruvia
