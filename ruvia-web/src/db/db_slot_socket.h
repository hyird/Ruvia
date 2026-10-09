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
struct db_slot_socket final {
    explicit db_slot_socket(asio::io_context& io_context);
    db_slot_socket(db_slot_socket&& other) noexcept;
    db_slot_socket& operator=(db_slot_socket&& other) noexcept;

    db_slot_socket(const db_slot_socket&) = delete;
    db_slot_socket& operator=(const db_slot_socket&) = delete;

#if defined(_WIN32)
    using native_socket_type = std::uintptr_t;
    asio::ip::tcp::socket socket_;
    static constexpr native_socket_type invalid_socket = std::numeric_limits<native_socket_type>::max();
#else
    using native_socket_type = asio::posix::stream_descriptor::native_handle_type;
    asio::posix::stream_descriptor descriptor_;
    static constexpr native_socket_type invalid_socket = -1;
#endif
    native_socket_type native_{invalid_socket};

    [[nodiscard]] std::error_code ensure_assigned(native_socket_type fd) noexcept;
    // Puts the driver-owned descriptor into non-blocking mode. A driver whose
    // asynchronous API suspends on EAGAIN cannot suspend at all while its
    // socket blocks, so it runs the whole operation inside the call that was
    // supposed to start it.
    [[nodiscard]] std::error_code make_non_blocking() noexcept;
    void cancel() noexcept;
    // Cancels pending waits and returns ownership to the database driver.
    // A failure leaves the wrapper attached so its caller can defer teardown
    // instead of risking a second close of the driver's handle.
    [[nodiscard]] std::error_code release() noexcept;
};

// Preallocated fallback for an unrecoverable Windows IOCP detach failure. The
// pool abandons a retained node for process lifetime instead of invoking either
// the wrapper destructor or driver cleanup while both know the same handle.
struct db_slot_socket_quarantine final {
    explicit db_slot_socket_quarantine(asio::io_context& io_context);

    void retain(db_slot_socket&& value, void* driver) noexcept;

    db_slot_socket socket_;
    void* driver_connection_{nullptr};
};

}  // namespace ruvia::detail
