#include "db/db_slot_socket.h"

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#endif

#include <system_error>
#include <utility>

namespace ruvia::detail {

#if defined(_WIN32)
db_slot_socket::db_slot_socket(asio::io_context& io_context)
    : socket_(io_context) {}

db_slot_socket::db_slot_socket(db_slot_socket&& other) noexcept
    : socket_(std::move(other.socket_)),
      source_(std::exchange(other.source_, invalid_socket)) {}
#else
db_slot_socket::db_slot_socket(asio::io_context& io_context)
    : descriptor_(io_context) {}

db_slot_socket::db_slot_socket(db_slot_socket&& other) noexcept
    : descriptor_(std::move(other.descriptor_)),
      source_(std::exchange(other.source_, invalid_socket)) {}
#endif

db_slot_socket& db_slot_socket::operator=(db_slot_socket&& other) noexcept {
    if (this == &other) {
        return *this;
    }
#if defined(_WIN32)
    socket_ = std::move(other.socket_);
#else
    descriptor_ = std::move(other.descriptor_);
#endif
    source_ = std::exchange(other.source_, invalid_socket);
    return *this;
}

db_slot_socket::waitable_type& db_slot_socket::waitable() noexcept {
#if defined(_WIN32)
    return socket_;
#else
    return descriptor_;
#endif
}

bool db_slot_socket::readiness_awaiter::await_suspend(std::coroutine_handle<> handle) noexcept {
    continuation_ = handle;
    try {
        // Count an operation only after Asio accepts it. If a later initiation
        // throws, cancel and stay suspended until the already-published
        // handlers have drained before surfacing the original exception.
        if (has_db_socket_readiness(wanted_, db_socket_readiness::read)) {
            start(waitable_type::wait_read, db_socket_readiness::read);
        }
        if (has_db_socket_readiness(wanted_, db_socket_readiness::write)) {
            start(waitable_type::wait_write, db_socket_readiness::write);
        }
        if (has_db_socket_readiness(wanted_, db_socket_readiness::error)) {
            start(waitable_type::wait_error, db_socket_readiness::error);
        }
    } catch (...) {
        initiation_failure_ = std::current_exception();
        completed_ = true;
        socket_.cancel();
    }
    return pending_ != 0;
}

void db_slot_socket::readiness_awaiter::start(waitable_type::wait_type wait_type, db_socket_readiness readiness) {
    socket_.waitable().async_wait(
        wait_type, [this, readiness](std::error_code wait_error) noexcept { complete(readiness, wait_error); });
    ++pending_;
}

void db_slot_socket::readiness_awaiter::complete(db_socket_readiness readiness, std::error_code error) noexcept {
    if (!completed_) {
        completed_ = true;
        result_.error_ = error;
        if (!error) {
            result_.ready_ = readiness;
        }
        if (pending_ > 1) {
            socket_.cancel();
        }
    }
    if (--pending_ == 0) {
        // Asio releases each wait operation before invoking its handler, so
        // once the last handler runs nothing still borrows this awaiter.
        continuation_.resume();
    }
}

db_socket_wait_result db_slot_socket::readiness_awaiter::await_resume() const {
    if (initiation_failure_ != nullptr) {
        std::rethrow_exception(initiation_failure_);
    }
    return result_;
}

std::error_code db_slot_socket::attach(native_socket_type driver_socket) noexcept {
    if (driver_socket == invalid_socket) {
        return std::make_error_code(std::errc::bad_file_descriptor);
    }
    if (source_ == driver_socket) {
        return {};
    }
    close();
    std::error_code ec;
#if defined(_WIN32)
    // WSADuplicateSocket is the documented way to duplicate a socket
    // (DuplicateHandle bypasses layered providers). Within one process the
    // result is a second handle to the same kernel socket object, so the IOCP
    // association made below covers the driver's handle too. The drivers only
    // issue synchronous, non-overlapped calls on it, which post no completion
    // packets.
    WSAPROTOCOL_INFOW protocol_info{};
    if (::WSADuplicateSocketW(static_cast<SOCKET>(driver_socket), ::GetCurrentProcessId(), &protocol_info) != 0) {
        return std::error_code(::WSAGetLastError(), std::system_category());
    }
    if (protocol_info.iSocketType != SOCK_STREAM ||
        (protocol_info.iAddressFamily != AF_INET && protocol_info.iAddressFamily != AF_INET6)) {
        return std::make_error_code(std::errc::address_family_not_supported);
    }
    const SOCKET duplicate = ::WSASocketW(FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO,
        &protocol_info, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
    if (duplicate == INVALID_SOCKET) {
        return std::error_code(::WSAGetLastError(), std::system_category());
    }
    socket_.assign(protocol_info.iAddressFamily == AF_INET ? asio::ip::tcp::v4() : asio::ip::tcp::v6(), duplicate, ec);
    if (ec) {
        (void)::closesocket(duplicate);
        return ec;
    }
#else
    const int duplicate = ::fcntl(driver_socket, F_DUPFD_CLOEXEC, 0);
    if (duplicate < 0) {
        return std::error_code(errno, std::system_category());
    }
    descriptor_.assign(duplicate, ec);
    if (ec) {
        (void)::close(duplicate);
        return ec;
    }
#endif
    source_ = driver_socket;
    return {};
}

void db_slot_socket::cancel() noexcept {
    std::error_code ignored;
    waitable().cancel(ignored);
}

void db_slot_socket::close() noexcept {
    std::error_code ec;
#if defined(_WIN32)
    // The duplicate is a second handle to the driver's socket object, and an
    // IOCP association belongs to that object rather than to a handle.
    // Closing only the duplicate would leave the driver's socket associated
    // with this io_context, and a later duplicate of the same socket could
    // not register. Dissociate first; if that fails, the plain close below
    // still runs and such a later registration reports the failure.
    if (socket_.is_open()) {
        const auto released = socket_.release(ec);
        if (!ec) {
            (void)::closesocket(released);
        }
    }
#endif
    // Asio invalidates its handle even when the close reports an error, and
    // the handle is ours alone, so there is nothing to retry.
    waitable().close(ec);
    source_ = invalid_socket;
}

std::error_code set_db_socket_non_blocking(db_slot_socket::native_socket_type driver_socket) noexcept {
#if defined(_WIN32)
    u_long non_blocking = 1;
    if (::ioctlsocket(static_cast<SOCKET>(driver_socket), FIONBIO, &non_blocking) != 0) {
        return std::error_code(::WSAGetLastError(), std::system_category());
    }
#else
    const int flags = ::fcntl(driver_socket, F_GETFL, 0);
    if (flags < 0 || ((flags & O_NONBLOCK) == 0 && ::fcntl(driver_socket, F_SETFL, flags | O_NONBLOCK) != 0)) {
        return std::error_code(errno, std::system_category());
    }
#endif
    return {};
}

}  // namespace ruvia::detail
