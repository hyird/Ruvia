#pragma once

#include <chrono>
#include <cstdint>
#include <system_error>

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

namespace ruvia::detail {

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
// the worker adopts into its Asio stream: a named pipe on Windows, an eventfd on
// Linux, a self-pipe on other POSIX systems.
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

}  // namespace ruvia::detail
