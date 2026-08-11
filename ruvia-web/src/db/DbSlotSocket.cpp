#include "ruvia/web/detail/db/DbSlotSocket.h"

#include <cerrno>
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

std::error_code DbSlotSocket::ensureAssigned(NativeSocket fd) noexcept {
    if (fd == kInvalidSocket) {
        return std::make_error_code(std::errc::bad_file_descriptor);
    }
    std::error_code ec;
#if defined(_WIN32)
    if (socket.is_open()) {
        if (native == fd) {
            return {};
        }
        reset();
    }
    WSAPROTOCOL_INFOW protocolInfo{};
    const auto source = static_cast<SOCKET>(fd);
    if (WSADuplicateSocketW(source, GetCurrentProcessId(), &protocolInfo) != 0) {
        return std::error_code(WSAGetLastError(), std::system_category());
    }
    if (protocolInfo.iAddressFamily != AF_INET && protocolInfo.iAddressFamily != AF_INET6) {
        return std::make_error_code(std::errc::address_family_not_supported);
    }
    const auto duplicate = WSASocketW(
        FROM_PROTOCOL_INFO,
        FROM_PROTOCOL_INFO,
        FROM_PROTOCOL_INFO,
        &protocolInfo,
        0,
        WSA_FLAG_OVERLAPPED);
    if (duplicate == INVALID_SOCKET) {
        return std::error_code(WSAGetLastError(), std::system_category());
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
            return {};
        }
        reset();
    }
    const auto duplicate = ::dup(fd);
    if (duplicate < 0) {
        return std::error_code(errno, std::system_category());
    }
    const auto descriptorFlags = ::fcntl(duplicate, F_GETFD);
    if (descriptorFlags < 0 || ::fcntl(duplicate, F_SETFD, descriptorFlags | FD_CLOEXEC) < 0) {
        const auto failure = std::error_code(errno, std::system_category());
        (void)::close(duplicate);
        return failure;
    }
    descriptor.assign(duplicate, ec);
    if (ec) {
        (void)::close(duplicate);
    }
#endif
    if (ec) {
        native = kInvalidSocket;
        return ec;
    }
    native = fd;
    return {};
}

std::error_code DbSlotSocket::makeNonBlocking() noexcept {
    std::error_code ec;
#if defined(_WIN32)
    socket.native_non_blocking(true, ec);
#else
    descriptor.native_non_blocking(true, ec);
#endif
    return ec;
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
