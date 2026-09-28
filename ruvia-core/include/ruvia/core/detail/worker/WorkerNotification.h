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

#include "ruvia/core/EventLoop.h"
#include "ruvia/core/WorkerNotification.h"

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
class WorkerRuntimeContext;
}

namespace ruvia::detail {

// One operation at a time uses this startup-owned storage as its Asio
// associated allocator. Asio destroys the operation (and deallocates this slot)
// before invoking its completion handler, so an immediately re-armed wait can
// reuse it without allocating or overlapping the previous operation.
class WorkerNotificationWaitResource final : public std::pmr::memory_resource {
public:
    [[nodiscard]] std::size_t allocationCount() const noexcept {
        return allocationCount_;
    }

    [[nodiscard]] std::size_t deallocationCount() const noexcept {
        return deallocationCount_;
    }

    [[nodiscard]] std::size_t outstandingAllocations() const noexcept {
        return allocated_ ? 1 : 0;
    }

private:
    static constexpr std::size_t kBufferSize = 1024;
    static constexpr std::size_t kMaxAlignment = 64;

    void* do_allocate(std::size_t bytes, std::size_t alignment) override;
    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) noexcept override;
    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override;

    alignas(kMaxAlignment) std::array<std::byte, kBufferSize> buffer_{};
    std::size_t allocationCount_{0};
    std::size_t deallocationCount_{0};
    void* allocatedPointer_{nullptr};
    std::size_t allocatedBytes_{0};
    std::size_t allocatedAlignment_{0};
    bool allocated_{false};
};

#ifdef _WIN32
using WorkerNotificationNativeHandle = HANDLE;
using WorkerNotificationReceiver = asio::windows::stream_handle;
#else
using WorkerNotificationNativeHandle = int;
using WorkerNotificationReceiver = asio::posix::stream_descriptor;
#endif

class WorkerNotificationState final {
public:
    WorkerNotificationState(EventLoop loop, WorkerNotificationOptions options);
    WorkerNotificationState(
        ::ruvia::WorkerRuntimeContext& runtime, WorkerNotificationOptions options);
    ~WorkerNotificationState();

    WorkerNotificationState(const WorkerNotificationState&) = delete;
    WorkerNotificationState& operator=(const WorkerNotificationState&) = delete;

    [[nodiscard]] WorkerNotificationStatus notify() noexcept;
    [[nodiscard]] bool waitReady();
    [[nodiscard]] bool beginWait(std::coroutine_handle<> continuation);
    [[nodiscard]] WorkerNotificationWaitStatus takeWaitResult();
    void close();

    [[nodiscard]] const WorkerNotificationWaitResource& waitResource() const noexcept {
        return waitResource_;
    }

#ifndef _WIN32
    [[nodiscard]] int senderDescriptor() const noexcept {
        return sender_;
    }
#endif

private:
    static constexpr std::uint64_t kClosed = std::uint64_t{1} << 63;
    static constexpr std::uint64_t kCountMask = ~kClosed;

    [[nodiscard]] bool enterProducer() noexcept;
    void leaveProducer() noexcept;
    [[nodiscard]] bool isClosed() const noexcept;
    void requireWorker() const;
    void initialize(WorkerNotificationOptions options);
    [[nodiscard]] asio::io_context& ioContext() const;
    void ensureReceiver();
    [[nodiscard]] bool drainReceiver(std::error_code& error) noexcept;
    void retireReceiver() noexcept;
    void completeWait(const std::error_code& error, std::size_t bytesTransferred) noexcept;

    EventLoop loop_;
    asio::io_context* runtimeIoContext_{nullptr};
    const WorkerHandle* runtimeHandle_{nullptr};
#ifdef _WIN32
    WorkerNotificationNativeHandle sender_{nullptr};
    WorkerNotificationNativeHandle receiver_{nullptr};
    std::array<char, 4096> readBuffer_{};
#else
    WorkerNotificationNativeHandle sender_{-1};
    WorkerNotificationNativeHandle receiver_{-1};
#endif
    std::atomic<std::uint64_t> activity_{0};
    std::atomic<bool> pending_{false};
    WorkerNotificationWaitResource waitResource_;
    std::optional<WorkerNotificationReceiver> receiverStream_;
    std::coroutine_handle<> waitContinuation_{};
    std::error_code waitError_;
    WorkerNotificationWaitStatus waitResult_{WorkerNotificationWaitStatus::kNotified};
    bool waitPending_{false};
};

}  // namespace ruvia::detail
