#pragma once

#ifdef _WIN32
#include <winsock2.h>
#include <asio/ip/tcp.hpp>
#else
#include <asio/posix/stream_descriptor.hpp>
#endif

#include <cstdint>
#include <limits>
#include <system_error>

namespace ruvia::detail {

// Transient ASIO wrapper around a database driver's native connection socket.
//
// MariaDB and libpq retain ownership of their native socket. ASIO borrows that
// socket only while a readiness wait is active. Every handler must drain and
// release() must detach ASIO before the driver is called again or closes it.
struct DbSlotSocket final {
    explicit DbSlotSocket(asio::io_context& ioContext);
    DbSlotSocket(DbSlotSocket&& other) noexcept;
    DbSlotSocket& operator=(DbSlotSocket&& other) noexcept;

    DbSlotSocket(const DbSlotSocket&) = delete;
    DbSlotSocket& operator=(const DbSlotSocket&) = delete;

#if defined(_WIN32)
    using NativeSocket = std::uintptr_t;
    asio::ip::tcp::socket socket;
    static constexpr NativeSocket kInvalidSocket = std::numeric_limits<NativeSocket>::max();
#else
    using NativeSocket = asio::posix::stream_descriptor::native_handle_type;
    asio::posix::stream_descriptor descriptor;
    static constexpr NativeSocket kInvalidSocket = -1;
#endif
    NativeSocket native{kInvalidSocket};

    [[nodiscard]] std::error_code ensureAssigned(NativeSocket fd) noexcept;
    // Puts the driver-owned descriptor into non-blocking mode. A driver whose
    // asynchronous API suspends on EAGAIN cannot suspend at all while its
    // socket blocks, so it runs the whole operation inside the call that was
    // supposed to start it.
    [[nodiscard]] std::error_code makeNonBlocking() noexcept;
    void cancel() noexcept;
    // Cancels pending waits and returns ownership to the database driver.
    // A failure leaves the wrapper attached so its caller can defer teardown
    // instead of risking a second close of the driver's handle.
    [[nodiscard]] std::error_code release() noexcept;
};

// Preallocated fallback for an unrecoverable Windows IOCP detach failure. The
// pool abandons a retained node for process lifetime instead of invoking either
// the wrapper destructor or driver cleanup while both know the same handle.
struct DbSlotSocketQuarantine final {
    explicit DbSlotSocketQuarantine(asio::io_context& ioContext);

    void retain(DbSlotSocket&& value, void* driver) noexcept;

    DbSlotSocket socket;
    void* driverConnection{nullptr};
};

}  // namespace ruvia::detail
