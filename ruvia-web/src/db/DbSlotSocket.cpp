#include "ruvia/web/detail/db/DbSlotSocket.h"

#include <system_error>

namespace ruvia::detail {

SlotSocket::SlotSocket(asio::io_context& ioContext)
#if defined(_WIN32)
    : socket(ioContext) {}
#else
    : descriptor(ioContext) {}
#endif

bool SlotSocket::ensureAssigned(my_socket fd) noexcept {
    if (fd == static_cast<my_socket>(MARIADB_INVALID_SOCKET)) {
        return false;
    }
    std::error_code ec;
#if defined(_WIN32)
    if (socket.is_open()) {
        if (native == fd) {
            return true;
        }
        (void)socket.release(ec);
    }
    socket.assign(asio::ip::tcp::v4(), fd, ec);
#else
    if (descriptor.is_open()) {
        if (native == fd) {
            return true;
        }
        (void)descriptor.release();
    }
    descriptor.assign(fd, ec);
#endif
    if (ec) {
        native = static_cast<my_socket>(MARIADB_INVALID_SOCKET);
        return false;
    }
    native = fd;
    return true;
}

void SlotSocket::cancel() noexcept {
    std::error_code ignored;
#if defined(_WIN32)
    socket.cancel(ignored);
#else
    descriptor.cancel(ignored);
#endif
}

void SlotSocket::release() noexcept {
    std::error_code ignored;
    (void)ignored;
#if defined(_WIN32)
    if (socket.is_open()) {
        (void)socket.release(ignored);
    }
#else
    if (descriptor.is_open()) {
        (void)descriptor.release();
    }
#endif
    native = static_cast<my_socket>(MARIADB_INVALID_SOCKET);
}

}  // namespace ruvia::detail
