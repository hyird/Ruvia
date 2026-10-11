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

// Transient ASIO wrapper around a database driver's native connection socket.
//
// MariaDB and libpq retain ownership of their native socket. ASIO borrows that
// socket only while a readiness wait is active. Every handler must drain and
// release() must detach ASIO before the driver is called again or closes it.
//
// The socket number is stable while a connection is up, but neither driver
// keeps it open: libmariadb (end_server) and libpq (pqDropConnection) close it
// inside their read/write calls when the transport fails. A registration kept
// across driver calls would then be detached from a closed -- and possibly
// reused -- handle (epoll_ctl DEL / IOCP FileReplaceCompletionInformation), so
// registration stays scoped to one wait for both drivers.
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
    native_socket_type native_{invalid_socket};

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

    [[nodiscard]] std::error_code ensure_assigned(native_socket_type fd) noexcept;
    // Puts the driver-owned descriptor into non-blocking mode. A driver whose
    // asynchronous API suspends on EAGAIN cannot suspend at all while its
    // socket blocks, so it runs the whole operation inside the call that was
    // supposed to start it.
    [[nodiscard]] std::error_code make_non_blocking() noexcept;
    [[nodiscard]] readiness_awaiter wait(db_socket_readiness wanted) noexcept {
        return readiness_awaiter(*this, wanted);
    }
    void cancel() noexcept;
    // Cancels pending waits and returns ownership to the database driver.
    // A failure leaves the wrapper attached so its caller can defer teardown
    // instead of risking a second close of the driver's handle.
    [[nodiscard]] std::error_code release() noexcept;

private:
    [[nodiscard]] waitable_type& waitable() noexcept;
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
