#pragma once

#include <array>
#include <atomic>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <system_error>

#include "ruvia/core/event_loop.h"
#include "ruvia/core/worker_notification.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <asio/windows/stream_handle.hpp>
#else
#include <asio/posix/stream_descriptor.hpp>
#endif

namespace ruvia {
class worker_runtime_context;
}

namespace ruvia::detail {

// One operation at a time uses this startup-owned storage as its Asio
// associated allocator. Asio destroys the operation (and deallocates this slot)
// before invoking its completion handler, so an immediately re-armed wait can
// reuse it without allocating or overlapping the previous operation.
class worker_notification_wait_resource final : public std::pmr::memory_resource {
public:
    [[nodiscard]] std::size_t allocation_count() const noexcept {
        return allocation_count_;
    }

    [[nodiscard]] std::size_t deallocation_count() const noexcept {
        return deallocation_count_;
    }

    [[nodiscard]] std::size_t outstanding_allocations() const noexcept {
        return allocated_ ? 1 : 0;
    }

private:
    static constexpr std::size_t buffer_size = 1024;
    static constexpr std::size_t max_alignment = 64;

    void* do_allocate(std::size_t bytes, std::size_t alignment) override;
    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) noexcept override;
    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override;

    std::array<std::byte, buffer_size + max_alignment - 1> buffer_storage_{};
    std::size_t allocation_count_{0};
    std::size_t deallocation_count_{0};
    void* allocated_pointer_{nullptr};
    std::size_t allocated_bytes_{0};
    std::size_t allocated_alignment_{0};
    bool allocated_{false};
};

#ifdef _WIN32
using worker_notification_native_handle_type = HANDLE;
using worker_notification_receiver_type = asio::windows::stream_handle;
#else
using worker_notification_native_handle_type = int;
using worker_notification_receiver_type = asio::posix::stream_descriptor;
#endif

class worker_notification_state final {
public:
    worker_notification_state(event_loop loop, worker_notification_options options);
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

    [[nodiscard]] const worker_notification_wait_resource& wait_resource() const noexcept {
        return wait_resource_;
    }

#ifndef _WIN32
    [[nodiscard]] int sender_descriptor() const noexcept {
        return sender_;
    }
#endif

private:
    static constexpr std::uint64_t closed = std::uint64_t{1} << 63;
    static constexpr std::uint64_t count_mask = ~closed;

    [[nodiscard]] bool enter_producer() noexcept;
    void leave_producer() noexcept;
    [[nodiscard]] bool is_closed() const noexcept;
    void require_worker() const;
    void initialize(worker_notification_options options);
    [[nodiscard]] asio::io_context& io_context() const;
    void ensure_receiver();
    [[nodiscard]] bool drain_receiver(std::error_code& error) noexcept;
    void retire_receiver() noexcept;
    void complete_wait(const std::error_code& error, std::size_t bytes_transferred) noexcept;

    event_loop loop_;
    asio::io_context* runtime_io_context_{nullptr};
    const worker_handle* runtime_handle_{nullptr};
#ifdef _WIN32
    worker_notification_native_handle_type sender_{nullptr};
    worker_notification_native_handle_type receiver_{nullptr};
    std::array<char, 4096> read_buffer_{};
#else
    worker_notification_native_handle_type sender_{-1};
    worker_notification_native_handle_type receiver_{-1};
#endif
    std::atomic<std::uint64_t> activity_{0};
    std::atomic<bool> pending_{false};
    worker_notification_wait_resource wait_resource_;
    std::optional<worker_notification_receiver_type> receiver_stream_;
    std::coroutine_handle<> wait_continuation_{};
    std::error_code wait_error_;
    worker_notification_wait_status wait_result_{worker_notification_wait_status::notified};
    bool wait_pending_{false};
};

}  // namespace ruvia::detail
