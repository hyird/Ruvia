#include "ruvia/web/detail/db/DbSlotSocket.h"

#include <system_error>

namespace ruvia::detail {

DbSlotSocket::DbSlotSocket(asio::io_context& ioContext)
    : descriptor(ioContext) {}

bool DbSlotSocket::ensureAssigned(NativeSocket fd) noexcept {
    if (fd == kInvalidSocket) {
        return false;
    }
    std::error_code ec;
    if (descriptor.is_open()) {
        if (native == fd) {
            return true;
        }
        if (!release()) {
            return false;
        }
    }
    descriptor.assign(fd, ec);
    if (ec) {
        native = kInvalidSocket;
        return false;
    }
    native = fd;
    return true;
}

void DbSlotSocket::cancel() noexcept {
    std::error_code ignored;
    descriptor.cancel(ignored);
}

bool DbSlotSocket::release() noexcept {
    try {
        if (descriptor.is_open()) {
            (void)descriptor.release();
        }
    } catch (...) {
        return false;
    }
    native = kInvalidSocket;
    return true;
}

}  // namespace ruvia::detail
