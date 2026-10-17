#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
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

#include "ruvia/core/worker_handle.h"
#include "ruvia/core/worker_notification.h"

namespace ruvia::detail {

// One operation at a time uses this startup-owned storage as its Asio
// associated allocator. Asio destroys the operation (and deallocates this slot)
// before invoking its completion handler, so an immediately re-armed wait can
// reuse it without allocating or overlapping the previous operation.
class worker_notification_wait_resource final : public std::pmr::memory_resource {
public:
    [[nodiscard]] bool allocated() const noexcept {
        return allocated_;
    }

private:
    static constexpr std::size_t buffer_size = 1024;
    static constexpr std::size_t max_alignment = 64;

    void* do_allocate(std::size_t bytes, std::size_t alignment) override;
    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) noexcept override;
    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override;

    std::array<std::byte, buffer_size + max_alignment - 1> buffer_storage_{};
    void* allocated_pointer_{nullptr};
    std::size_t allocated_bytes_{0};
    std::size_t allocated_alignment_{0};
    bool allocated_{false};
};

#ifdef _WIN32
using native_wake_handle = HANDLE;
using native_wake_stream = asio::windows::stream_handle;
inline constexpr native_wake_handle invalid_native_wake_handle = nullptr;
#else
using native_wake_handle = int;
using native_wake_stream = asio::posix::stream_descriptor;
inline constexpr native_wake_handle invalid_native_wake_handle = -1;
#endif

enum class native_wake_signal : std::uint8_t {
    signaled,
    full,
    failed,
};

// The platform wake channel: a nonblocking sender and the receiver handle that
// the worker adopts into its Asio stream. Implemented by
// worker_notification_win32.cpp (named pipe) and worker_notification_posix.cpp
// (Linux eventfd or a POSIX self-pipe).
struct native_wake_pair final {
    native_wake_handle sender_{invalid_native_wake_handle};
    native_wake_handle receiver_{invalid_native_wake_handle};
#if !defined(_WIN32) && !defined(__linux__)
    // Keep a reader alive through close/notify races, preventing SIGPIPE until
    // all producers have retired and the pair itself can be destroyed.
    native_wake_handle receiver_guard_{invalid_native_wake_handle};
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
void disconnect_native_receiver(native_wake_handle receiver) noexcept;
#else
// Consumes native wake tokens; returns whether a notification was pending.
[[nodiscard]] bool drain_native_receiver(native_wake_handle receiver, std::error_code& error) noexcept;
#endif

class worker_notification_state final {
public:
    worker_notification_state(
        ::ruvia::worker_runtime_context& runtime, worker_notification_options options);
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
    const worker_handle& worker_;
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
