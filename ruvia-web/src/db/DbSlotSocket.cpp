#include "ruvia/web/detail/db/DbSlotSocket.h"

#include <system_error>

#if defined(_WIN32)
#include <processthreadsapi.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace ruvia::detail {

DbSlotSocket::DbSlotSocket(asio::io_context& ioContext)
#if defined(_WIN32)
    : socket(ioContext){}
#else
    : descriptor(ioContext) {
}
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
        reset();
    }
    WSAPROTOCOL_INFOW protocolInfo{};
    const auto source = static_cast<SOCKET>(fd);
    if (WSADuplicateSocketW(source, GetCurrentProcessId(), &protocolInfo) != 0) {
        return false;
    }
    if (protocolInfo.iAddressFamily != AF_INET && protocolInfo.iAddressFamily != AF_INET6) {
        return false;
    }
    const auto duplicate = WSASocketW(
        FROM_PROTOCOL_INFO,
        FROM_PROTOCOL_INFO,
        FROM_PROTOCOL_INFO,
        &protocolInfo,
        0,
        WSA_FLAG_OVERLAPPED);
    if (duplicate == INVALID_SOCKET) {
        return false;
    }
    const auto protocol = protocolInfo.iAddressFamily == AF_INET6
        ? asio::ip::tcp::v6()
        : asio::ip::tcp::v4();
    socket.assign(protocol, duplicate, ec);
    if (ec) {
        (void)closesocket(duplicate);
    }
#else
    if (descriptor.is_open()) {
        if (native == fd) {
            return true;
        }
        reset();
    }
    const auto duplicate = ::dup(fd);
    if (duplicate < 0) {
        return false;
    }
    const auto descriptorFlags = ::fcntl(duplicate, F_GETFD);
    if (descriptorFlags < 0 || ::fcntl(duplicate, F_SETFD, descriptorFlags | FD_CLOEXEC) < 0) {
        (void)::close(duplicate);
        return false;
    }
    descriptor.assign(duplicate, ec);
    if (ec) {
        (void)::close(duplicate);
    }
#endif
    if (ec) {
        native = kInvalidSocket;
        return false;
    }
    native = fd;
    return true;
}

bool DbSlotSocket::makeNonBlocking() noexcept {
    std::error_code ec;
#if defined(_WIN32)
    socket.native_non_blocking(true, ec);
#else
    descriptor.native_non_blocking(true, ec);
#endif
    return !ec;
}

void DbSlotSocket::cancel() noexcept {
    std::error_code ignored;
#if defined(_WIN32)
    socket.cancel(ignored);
#else
    descriptor.cancel(ignored);
#endif
}

void DbSlotSocket::reset() noexcept {
    std::error_code ignored;
#if defined(_WIN32)
    socket.close(ignored);
#else
    descriptor.close(ignored);
#endif
    native = kInvalidSocket;
}

}  // namespace ruvia::detail
