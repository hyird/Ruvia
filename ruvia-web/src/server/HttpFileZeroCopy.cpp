#include "ruvia/web/detail/server/HttpFileZeroCopy.h"

#include "ruvia/core/detail/AsioAwait.h"
#include "ruvia/http/detail/file/HttpNativeFile.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

#if defined(__linux__)
#include <cerrno>
#include <sys/sendfile.h>
#elif defined(_WIN32)
#include <mswsock.h>
#endif

namespace ruvia::detail {

#if defined(__linux__)
Task<void> writeFileZeroCopy(asio::ip::tcp::socket& socket, ResponseFileBody file, std::error_code& ec) {
    auto input = openNativeFileForRead(file, ec);
    if (ec) { co_return; }
    auto offset = static_cast<off_t>(file.offset);
    std::uint64_t remaining = file.length;
    while (remaining > 0) {
        const auto nextSend = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, 0x7ffff000ULL));
        const auto sent = ::sendfile(socket.native_handle(), input.get(), &offset, nextSend);
        if (sent > 0) { remaining -= static_cast<std::uint64_t>(sent); continue; }
        if (sent == 0) { ec = asio::error::operation_aborted; co_return; }
        if (errno == EINTR) { continue; }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            ec = co_await asyncError([&socket](auto handler) mutable { socket.async_wait(asio::ip::tcp::socket::wait_write, std::move(handler)); });
            if (ec) { co_return; }
            continue;
        }
        ec = std::error_code(errno, std::system_category());
        co_return;
    }
    ec = {};
}
#elif defined(_WIN32)
Task<void> writeFileZeroCopy(asio::ip::tcp::socket& socket, ResponseFileBody file, std::error_code& ec) {
    auto input = openNativeFileForRead(file, ec);
    if (ec) { co_return; }
    LARGE_INTEGER position; position.QuadPart = static_cast<LONGLONG>(file.offset);
    if (::SetFilePointerEx(input.get(), position, nullptr, FILE_BEGIN) == 0) { ec = std::error_code(static_cast<int>(::GetLastError()), std::system_category()); co_return; }
    std::uint64_t remaining = file.length;
    while (remaining > 0) {
        const auto nextSend = static_cast<DWORD>(std::min<std::uint64_t>(remaining, static_cast<std::uint64_t>((std::numeric_limits<DWORD>::max)())));
        if (::TransmitFile(socket.native_handle(), input.get(), nextSend, 0, nullptr, nullptr, 0) != FALSE) { remaining -= nextSend; continue; }
        const auto error = ::WSAGetLastError();
        if (error == WSAEWOULDBLOCK) {
            ec = co_await asyncError([&socket](auto handler) mutable { socket.async_wait(asio::ip::tcp::socket::wait_write, std::move(handler)); });
            if (ec) { co_return; }
            continue;
        }
        ec = std::error_code(error, std::system_category());
        co_return;
    }
    ec = {};
}
#else
Task<void> writeFileZeroCopy(asio::ip::tcp::socket&, ResponseFileBody, std::error_code& ec) {
    ec = asio::error::operation_not_supported;
    co_return;
}
#endif

}  // namespace ruvia::detail
