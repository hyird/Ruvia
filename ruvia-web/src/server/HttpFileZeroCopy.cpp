#include "ruvia/web/detail/server/HttpFileZeroCopy.h"

#include "ruvia/core/detail/AsioAwait.h"
#include "ruvia/web/detail/server/HttpNativeFile.h"

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
Task<HttpFileZeroCopyResult> writeFileZeroCopy(
    asio::ip::tcp::socket& socket,
    ResponseFileBody file) {
    std::error_code error;
    auto input = openNativeFileForRead(file, error);
    if (error) {
        co_return HttpFileZeroCopyResult::makeFailed(error);
    }
    auto offset = static_cast<off_t>(file.offset());
    std::uint64_t remaining = file.length();
    while (remaining > 0) {
        const auto nextSend = static_cast<std::size_t>(
            std::min<std::uint64_t>(remaining, 0x7ffff000ULL));
        const auto sent = ::sendfile(
            socket.native_handle(),
            input.get(),
            &offset,
            nextSend);
        if (sent > 0) {
            remaining -= static_cast<std::uint64_t>(sent);
            continue;
        }
        if (sent == 0) {
            co_return HttpFileZeroCopyResult::makeFailed(
                asio::error::operation_aborted);
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            error = co_await asyncError(
                [&socket](auto handler) mutable {
                    socket.async_wait(
                        asio::ip::tcp::socket::wait_write,
                        std::move(handler));
                });
            if (error) {
                co_return HttpFileZeroCopyResult::makeFailed(error);
            }
            continue;
        }
        co_return HttpFileZeroCopyResult::makeFailed(
            std::error_code(errno, std::system_category()));
    }
    co_return HttpFileZeroCopyResult::makeCompleted();
}
#elif defined(_WIN32)
Task<HttpFileZeroCopyResult> writeFileZeroCopy(
    asio::ip::tcp::socket& socket,
    ResponseFileBody file) {
    std::error_code error;
    auto input = openNativeFileForRead(file, error);
    if (error) {
        co_return HttpFileZeroCopyResult::makeFailed(error);
    }
    LARGE_INTEGER position;
    position.QuadPart = static_cast<LONGLONG>(file.offset());
    if (::SetFilePointerEx(input.get(), position, nullptr, FILE_BEGIN) == 0) {
        co_return HttpFileZeroCopyResult::makeFailed(std::error_code(
            static_cast<int>(::GetLastError()),
            std::system_category()));
    }
    std::uint64_t remaining = file.length();
    while (remaining > 0) {
        const auto nextSend = static_cast<DWORD>(
            std::min<std::uint64_t>(
                remaining,
                static_cast<std::uint64_t>(
                    (std::numeric_limits<DWORD>::max)())));
        if (::TransmitFile(
                socket.native_handle(),
                input.get(),
                nextSend,
                0,
                nullptr,
                nullptr,
                0) != FALSE) {
            remaining -= nextSend;
            continue;
        }
        const auto socketError = ::WSAGetLastError();
        if (socketError == WSAEWOULDBLOCK) {
            const auto waitError = co_await asyncError(
                [&socket](auto handler) mutable {
                    socket.async_wait(
                        asio::ip::tcp::socket::wait_write,
                        std::move(handler));
                });
            if (waitError) {
                co_return HttpFileZeroCopyResult::makeFailed(waitError);
            }
            continue;
        }
        co_return HttpFileZeroCopyResult::makeFailed(
            std::error_code(socketError, std::system_category()));
    }
    co_return HttpFileZeroCopyResult::makeCompleted();
}
#else
Task<HttpFileZeroCopyResult> writeFileZeroCopy(
    asio::ip::tcp::socket&,
    ResponseFileBody) {
    co_return HttpFileZeroCopyResult::makeUnavailable();
}
#endif

}  // namespace ruvia::detail
