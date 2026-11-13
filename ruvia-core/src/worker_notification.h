#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <system_error>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <asio/io_context.hpp>
#ifdef _WIN32
#include <asio/windows/stream_handle.hpp>
#else
#include <asio/posix/stream_descriptor.hpp>
#endif

#include "ruvia/core/worker_notification.h"
#include "ruvia/core/worker_ref.h"

namespace ruvia::detail {

// One operation at a time uses this startup-owned storage as its Asio
// associated allocator. Asio destroys the operation (and deallocates this slot)
// before invoking its completion handler, so an immediately re-armed wait can
// reuse it without allocating or overlapping the previous operation.
class worker_notification_wait_resource final {
public:
    [[nodiscard]] bool allocated() const noexcept {
        return allocation_ != nullptr;
    }
    void* allocate(std::size_t bytes, std::size_t alignment);
    void deallocate(void* pointer, std::size_t bytes, std::size_t alignment) noexcept;

private:
    static constexpr std::size_t buffer_size = 1024;
    static constexpr std::size_t max_alignment = 64;

    // Leave room to align the operation without over-aligning the resource or its owner.
    std::array<std::byte, buffer_size + max_alignment - 1> buffer_storage_{};
    void* allocation_{nullptr};
};

template <typename t_type>
class worker_notification_wait_allocator final {
public:
    using value_type = t_type;

    explicit worker_notification_wait_allocator(
        worker_notification_wait_resource& resource) noexcept
        : resource_(&resource) {}

    template <typename other_type>
    worker_notification_wait_allocator(
        const worker_notification_wait_allocator<other_type>& other) noexcept
        : resource_(other.resource_) {}

    [[nodiscard]] t_type* allocate(std::size_t count) const {
        return static_cast<t_type*>(resource_->allocate(count * sizeof(t_type), alignof(t_type)));
    }

    void deallocate(t_type* pointer, std::size_t count) const noexcept {
        resource_->deallocate(pointer, count * sizeof(t_type), alignof(t_type));
    }

    template <typename other_type>
    [[nodiscard]] bool operator==(
        const worker_notification_wait_allocator<other_type>& other) const noexcept {
        return resource_ == other.resource_;
    }

private:
    template <typename>
    friend class worker_notification_wait_allocator;

    worker_notification_wait_resource* resource_;
};

#ifdef _WIN32
using native_wake_descriptor = HANDLE;
using native_wake_stream = asio::windows::stream_handle;
inline constexpr native_wake_descriptor invalid_native_wake_descriptor = nullptr;
#else
using native_wake_descriptor = int;
using native_wake_stream = asio::posix::stream_descriptor;
inline constexpr native_wake_descriptor invalid_native_wake_descriptor = -1;
#endif

enum class native_wake_signal : std::uint8_t {
    signaled,
    full,
    failed,
};

// The platform wake channel: a nonblocking sender and the receiver descriptor that
// the worker adopts into its Asio stream. Implemented by
// worker_notification_win32.cpp (named pipe) and worker_notification_posix.cpp
// (Linux eventfd or a POSIX self-pipe).
struct native_wake_pair final {
    native_wake_descriptor sender_{invalid_native_wake_descriptor};
    native_wake_descriptor receiver_{invalid_native_wake_descriptor};
#if !defined(_WIN32) && !defined(__linux__)
    // Keep a reader alive through close/notify races, preventing SIGPIPE until
    // all producers have retired and the pair itself can be destroyed.
    native_wake_descriptor receiver_guard_{invalid_native_wake_descriptor};
#endif

    native_wake_pair() = default;
    native_wake_pair(const native_wake_pair&) = delete;
    native_wake_pair& operator=(const native_wake_pair&) = delete;
    native_wake_pair(native_wake_pair&& other) noexcept;
    native_wake_pair& operator=(native_wake_pair&& other) noexcept;
    ~native_wake_pair();

    // Safe from any thread; writes one token without blocking.
    [[nodiscard]] native_wake_signal signal() const noexcept;
    void close_receiver() noexcept;
};

// startup_timeout bounds the Windows pipe connection; POSIX setup ignores it.
[[nodiscard]] native_wake_pair make_native_wake_pair(std::chrono::milliseconds startup_timeout);

[[noreturn]] void throw_native_wake_error(const char* operation, int error);

#ifdef _WIN32
void disconnect_native_receiver(native_wake_descriptor receiver) noexcept;
#else
// Consumes native wake tokens; returns whether a notification was pending.
[[nodiscard]] bool drain_native_receiver(native_wake_descriptor receiver, std::error_code& error) noexcept;
#endif

class worker_notification_state final {
public:
    worker_notification_state(asio::io_context& io_context, const worker_ref& worker,
        worker_notification_options options);
    ~worker_notification_state();

    worker_notification_state(const worker_notification_state&) = delete;
    worker_notification_state& operator=(const worker_notification_state&) = delete;

    [[nodiscard]] worker_notification_status notify() noexcept;
    [[nodiscard]] bool wait_ready();
    [[nodiscard]] bool begin_wait(std::coroutine_handle<> continuation);
    [[nodiscard]] worker_notification_wait_status take_wait_result();
    void close();

private:
    static constexpr std::uint64_t closed = std::uint64_t{1} << 63;
    static constexpr std::uint64_t count_mask = ~closed;

    [[nodiscard]] bool enter_producer() noexcept;
    void leave_producer() noexcept;
    [[nodiscard]] bool is_closed() const noexcept;
    void require_worker() const;
    void ensure_receiver();
#ifndef _WIN32
    [[nodiscard]] bool drain_receiver(std::error_code& error) noexcept;
#endif
    void retire_receiver() noexcept;
    void complete_wait(const std::error_code& error, std::size_t bytes_transferred) noexcept;

    asio::io_context& io_context_;
    const worker_ref& worker_;
    native_wake_pair wake_;
#ifdef _WIN32
    std::array<char, 4096> read_buffer_{};
#endif
    std::atomic<std::uint64_t> activity_{0};
    std::atomic<bool> pending_{false};
    worker_notification_wait_resource wait_resource_;
    std::optional<native_wake_stream> receiver_stream_;
    std::coroutine_handle<> wait_continuation_{};
    std::error_code wait_error_;
    worker_notification_wait_status wait_result_{worker_notification_wait_status::notified};
    bool wait_pending_{false};
};

}  // namespace ruvia::detail
