#pragma once

#ifdef _WIN32
#include <winsock2.h>

#include <asio/ip/tcp.hpp>
#else
#include <asio/posix/stream_descriptor.hpp>
#endif

#include <coroutine>
#include <cstdint>
#include <exception>
#include <limits>
#include <system_error>

namespace ruvia::detail {

// Readiness classes a database driver can wait for. A wait completes on the
// first class that becomes ready.
enum class db_socket_readiness : std::uint8_t {
    none = 0,
    read = 1,
    write = 2,
    error = 4,
};

[[nodiscard]] constexpr db_socket_readiness operator|(db_socket_readiness left, db_socket_readiness right) noexcept {
    return static_cast<db_socket_readiness>(static_cast<std::uint8_t>(left) | static_cast<std::uint8_t>(right));
}

[[nodiscard]] constexpr bool has_db_socket_readiness(db_socket_readiness set, db_socket_readiness value) noexcept {
    return (static_cast<std::uint8_t>(set) & static_cast<std::uint8_t>(value)) != 0;
}

struct db_socket_wait_result final {
    // The class that completed first; none when the wait failed or was cancelled.
    db_socket_readiness ready_{db_socket_readiness::none};
    std::error_code error_{};
    // The slot deadline guarding this wait expired.
    bool expired_{false};
};

// Ruvia-owned duplicate of a database driver's connection socket, registered
// with ASIO for readiness waits.
//
// MariaDB and libpq own their native socket and close it themselves inside
// their read/write calls when the transport fails (libmariadb end_server,
// libpq pqDropConnection). Registering that handle directly would leave ASIO
// holding a handle the driver may close -- and the OS may reuse -- between two
// waits. A duplicate refers to the same socket object, so it observes the same
// readiness, but its handle belongs to Ruvia: the driver closing its own
// handle can neither invalidate nor be invalidated by it. The duplicate is
// therefore made and registered once per driver socket and kept until the
// pool closes the connection.
//
// The duplicate keeps the underlying connection open after the driver has
// closed its handle. The driver only does that on a transport failure, which
// the pool answers by closing the slot, and closing the slot closes the
// duplicate first so the driver's own close still ends the connection.
struct db_slot_socket final {
    explicit db_slot_socket(asio::io_context& io_context);
    db_slot_socket(db_slot_socket&& other) noexcept;
    db_slot_socket& operator=(db_slot_socket&& other) noexcept;

    db_slot_socket(const db_slot_socket&) = delete;
    db_slot_socket& operator=(const db_slot_socket&) = delete;

#if defined(_WIN32)
    using native_socket_type = std::uintptr_t;
    using waitable_type = asio::ip::tcp::socket;
    asio::ip::tcp::socket socket_;
    static constexpr native_socket_type invalid_socket = std::numeric_limits<native_socket_type>::max();
#else
    using native_socket_type = asio::posix::stream_descriptor::native_handle_type;
    using waitable_type = asio::posix::stream_descriptor;
    asio::posix::stream_descriptor descriptor_;
    static constexpr native_socket_type invalid_socket = -1;
#endif
    // The driver handle the open duplicate was made from.
    native_socket_type source_{invalid_socket};

    // Any-of readiness wait. The first completion cancels the others, and the
    // awaiting coroutine resumes only after every started handler has run, so
    // no outstanding operation still borrows the awaiter.
    class readiness_awaiter final {
    public:
        readiness_awaiter(db_slot_socket& socket, db_socket_readiness wanted) noexcept
            : socket_(socket),
              wanted_(wanted) {}

        [[nodiscard]] bool await_ready() const noexcept {
            return false;
        }
        [[nodiscard]] bool await_suspend(std::coroutine_handle<> handle) noexcept;
        // Rethrows a failure to start a wait.
        [[nodiscard]] db_socket_wait_result await_resume() const;

    private:
        void start(waitable_type::wait_type wait_type, db_socket_readiness readiness);
        void complete(db_socket_readiness readiness, std::error_code error) noexcept;

        db_slot_socket& socket_;
        db_socket_readiness wanted_;
        std::coroutine_handle<> continuation_{};
        db_socket_wait_result result_{};
        std::exception_ptr initiation_failure_{};
        std::uint8_t pending_{0};
        bool completed_{false};
    };

    // Duplicates and registers the driver's socket unless the open duplicate
    // was already made from this handle value. The value is the only identity
    // checked, so a caller must close() before the driver can replace its
    // socket with one that reuses the value.
    [[nodiscard]] std::error_code attach(native_socket_type driver_socket) noexcept;
    [[nodiscard]] readiness_awaiter wait(db_socket_readiness wanted) noexcept {
        return readiness_awaiter(*this, wanted);
    }
    void cancel() noexcept;
    // Cancels pending waits and closes the duplicate, leaving the driver's
    // socket open and unregistered.
    void close() noexcept;

private:
    [[nodiscard]] waitable_type& waitable() noexcept;
};

// Puts a driver-owned socket into non-blocking mode. A driver whose
// asynchronous API suspends on EAGAIN cannot suspend at all while its socket
// blocks, so it runs the whole operation inside the call that was supposed to
// start it.
[[nodiscard]] std::error_code set_db_socket_non_blocking(db_slot_socket::native_socket_type driver_socket) noexcept;

}  // namespace ruvia::detail
