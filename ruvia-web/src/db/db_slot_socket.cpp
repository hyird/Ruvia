#include "db/db_slot_socket.h"

#include <system_error>
#include <utility>

namespace ruvia::detail {

db_slot_socket::db_slot_socket(asio::io_context& io_context)
#if defined(_WIN32)
    : socket_(io_context){}
#else
    : descriptor_(io_context) {
}
#endif

      db_slot_socket::db_slot_socket(db_slot_socket && other) noexcept
#if defined(_WIN32)
    : socket_(std::move(other.socket_)),
#else
    : descriptor_(std::move(other.descriptor_)),
#endif
      native_(std::exchange(other.native_, invalid_socket)) {
}

db_slot_socket& db_slot_socket::operator=(db_slot_socket&& other) noexcept {
    if (this == &other) {
        return *this;
    }
#if defined(_WIN32)
    socket_ = std::move(other.socket_);
#else
    descriptor_ = std::move(other.descriptor_);
#endif
    native_ = std::exchange(other.native_, invalid_socket);
    return *this;
}

std::error_code db_slot_socket::ensure_assigned(native_socket_type fd) noexcept {
    if (fd == invalid_socket) {
        return std::make_error_code(std::errc::bad_file_descriptor);
    }
    std::error_code ec;
#if defined(_WIN32)
    if (socket_.is_open()) {
        if (native_ == fd) {
            return {};
        }
        if (const auto release_error = release(); release_error) {
            return release_error;
        }
    }
    WSAPROTOCOL_INFOW protocol_info{};
    int protocol_info_size = static_cast<int>(sizeof(protocol_info));
    const auto source_value = static_cast<SOCKET>(fd);
    if (::getsockopt(source_value, SOL_SOCKET, SO_PROTOCOL_INFOW, reinterpret_cast<char*>(&protocol_info),
            &protocol_info_size) == SOCKET_ERROR) {
        return std::error_code(WSAGetLastError(), std::system_category());
    }
    if (protocol_info.iAddressFamily == AF_INET) {
        socket_.assign(asio::ip::tcp::v4(), source_value, ec);
    } else if (protocol_info.iAddressFamily == AF_INET6) {
        socket_.assign(asio::ip::tcp::v6(), source_value, ec);
    } else {
        return std::make_error_code(std::errc::address_family_not_supported);
    }
#else
    if (descriptor_.is_open()) {
        if (native_ == fd) {
            return {};
        }
        if (const auto release_error = release(); release_error) {
            return release_error;
        }
    }
    descriptor_.assign(fd, ec);
#endif
    if (ec) {
        native_ = invalid_socket;
        return ec;
    }
    native_ = fd;
    return {};
}

std::error_code db_slot_socket::make_non_blocking() noexcept {
    std::error_code ec;
#if defined(_WIN32)
    socket_.native_non_blocking(true, ec);
#else
    descriptor_.native_non_blocking(true, ec);
#endif
    return ec;
}

void db_slot_socket::cancel() noexcept {
    std::error_code ignored;
#if defined(_WIN32)
    socket_.cancel(ignored);
#else
    descriptor_.cancel(ignored);
#endif
}

std::error_code db_slot_socket::release() noexcept {
#if defined(_WIN32)
    if (socket_.is_open()) {
        std::error_code ec;
        (void)socket_.release(ec);
        if (ec) {
            return ec;
        }
    }
#else
    try {
        if (descriptor_.is_open()) {
            (void)descriptor_.release();
        }
    } catch (const std::system_error& error) {
        return error.code();
    } catch (...) {
        return std::make_error_code(std::errc::io_error);
    }
#endif
    native_ = invalid_socket;
    return {};
}

db_slot_socket_quarantine::db_slot_socket_quarantine(asio::io_context& io_context)
    : socket_(io_context) {}

void db_slot_socket_quarantine::retain(db_slot_socket&& value, void* driver) noexcept {
    socket_ = std::move(value);
    driver_connection_ = driver;
}

}  // namespace ruvia::detail
