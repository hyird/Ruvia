#include "ruvia/web/detail/db/DbSlotSocket.h"

#include <system_error>
#include <utility>

namespace ruvia::detail {

DbSlotSocket::DbSlotSocket(asio::io_context& ioContext)
#if defined(_WIN32)
    : socket(ioContext){}
#else
    : descriptor(ioContext) {
}
#endif

      DbSlotSocket::DbSlotSocket(DbSlotSocket && other) noexcept
#if defined(_WIN32)
    : socket(std::move(other.socket)),
#else
    : descriptor(std::move(other.descriptor)),
#endif
      native(std::exchange(other.native, kInvalidSocket)) {
}

DbSlotSocket& DbSlotSocket::operator=(DbSlotSocket&& other) noexcept {
    if (this == &other) {
        return *this;
    }
#if defined(_WIN32)
    socket = std::move(other.socket);
#else
    descriptor = std::move(other.descriptor);
#endif
    native = std::exchange(other.native, kInvalidSocket);
    return *this;
}

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
        if (const auto releaseError = release(); releaseError) {
            return releaseError;
        }
    }
    WSAPROTOCOL_INFOW protocolInfo{};
    int protocolInfoSize = static_cast<int>(sizeof(protocolInfo));
    const auto source = static_cast<SOCKET>(fd);
    if (::getsockopt(source, SOL_SOCKET, SO_PROTOCOL_INFOW, reinterpret_cast<char*>(&protocolInfo),
            &protocolInfoSize) == SOCKET_ERROR) {
        return std::error_code(WSAGetLastError(), std::system_category());
    }
    if (protocolInfo.iAddressFamily == AF_INET) {
        socket.assign(asio::ip::tcp::v4(), source, ec);
    } else if (protocolInfo.iAddressFamily == AF_INET6) {
        socket.assign(asio::ip::tcp::v6(), source, ec);
    } else {
        return std::make_error_code(std::errc::address_family_not_supported);
    }
#else
    if (descriptor.is_open()) {
        if (native == fd) {
            return {};
        }
        if (const auto releaseError = release(); releaseError) {
            return releaseError;
        }
    }
    descriptor.assign(fd, ec);
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

std::error_code DbSlotSocket::release() noexcept {
#if defined(_WIN32)
    if (socket.is_open()) {
        std::error_code ec;
        (void)socket.release(ec);
        if (ec) {
            return ec;
        }
    }
#else
    try {
        if (descriptor.is_open()) {
            (void)descriptor.release();
        }
    } catch (const std::system_error& error) {
        return error.code();
    } catch (...) {
        return std::make_error_code(std::errc::io_error);
    }
#endif
    native = kInvalidSocket;
    return {};
}

DbSlotSocketQuarantine::DbSlotSocketQuarantine(asio::io_context& ioContext)
    : socket(ioContext) {}

void DbSlotSocketQuarantine::retain(DbSlotSocket&& value, void* driver) noexcept {
    socket = std::move(value);
    driverConnection = driver;
}

}  // namespace ruvia::detail
