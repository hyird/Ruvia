#include "ruvia/web/detail/db/DbSlotSocket.h"

#include <system_error>

namespace ruvia::detail {

DbSlotSocket::DbSlotSocket(asio::io_context& ioContext)
#if defined(_WIN32)
    : socket(ioContext) {}
#else
    : descriptor(ioContext) {}
#endif

bool DbSlotSocket::ensureAssigned(NativeSocket fd) noexcept {
    if (fd == kInvalidSocket) {
        return false;
    }
    std::error_code ec;
#if defined(_WIN32)
    if (socket.is_open()) {
        if (native == fd) {
            return true;
        }
        if (!release()) {
            return false;
        }
    }
    socket.assign(
        asio::ip::tcp::v4(),
        static_cast<asio::ip::tcp::socket::native_handle_type>(fd),
        ec);
#else
    if (descriptor.is_open()) {
        if (native == fd) {
            return true;
        }
        if (!release()) {
            return false;
        }
    }
    descriptor.assign(fd, ec);
#endif
    if (ec) {
        native = kInvalidSocket;
        return false;
    }
    native = fd;
    return true;
}

void DbSlotSocket::cancel() noexcept {
    std::error_code ignored;
#if defined(_WIN32)
    socket.cancel(ignored);
#else
    descriptor.cancel(ignored);
#endif
}

bool DbSlotSocket::release() noexcept {
#if defined(_WIN32)
    if (socket.is_open()) {
        std::error_code ec;
        (void)socket.release(ec);
        if (ec) {
            return false;
        }
    }
#else
    try {
        if (descriptor.is_open()) {
            (void)descriptor.release();
        }
    } catch (...) {
        return false;
    }
#endif
    native = kInvalidSocket;
    return true;
}

}  // namespace ruvia::detail
