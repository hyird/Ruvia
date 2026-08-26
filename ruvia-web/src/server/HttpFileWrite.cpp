#include "ruvia/web/detail/server/file/HttpFileWrite.h"

#include "ruvia/core/detail/io/AsioAwait.h"
#include "ruvia/web/detail/server/file/HttpNativeFile.h"

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

Task<std::error_code> writeHttpResponseFile(asio::ip::tcp::socket& socket, WorkerMemory& memory,
    std::pmr::string* reusableChunk, ResponseFileBody file) {
#if defined(__linux__)
    static_cast<void>(memory);
    static_cast<void>(reusableChunk);
    std::error_code error;
    auto input = openNativeFileForRead(file, error);
    if (error) {
        co_return error;
    }
    auto offset = static_cast<off_t>(file.offset());
    std::uint64_t remaining = file.length();
    while (remaining > 0) {
        const auto nextSend =
            static_cast<std::size_t>(std::min<std::uint64_t>(remaining, 0x7ffff000ULL));
        const auto sent = ::sendfile(socket.native_handle(), input.get(), &offset, nextSend);
        if (sent > 0) {
            remaining -= static_cast<std::uint64_t>(sent);
            continue;
        }
        if (sent == 0) {
            co_return asio::error::operation_aborted;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            const auto waitCompletion = co_await asyncAsio([&socket](auto handler) mutable {
                socket.async_wait(asio::ip::tcp::socket::wait_write, std::move(handler));
            });
            error = waitCompletion.errorCode();
            if (error) {
                co_return error;
            }
            continue;
        }
        co_return std::error_code(errno, std::system_category());
    }
    co_return std::error_code{};
#elif defined(_WIN32)
    static_cast<void>(memory);
    static_cast<void>(reusableChunk);
    std::error_code error;
    auto input = openNativeFileForRead(file, error);
    if (error) {
        co_return error;
    }
    LARGE_INTEGER position;
    position.QuadPart = static_cast<LONGLONG>(file.offset());
    if (::SetFilePointerEx(input.get(), position, nullptr, FILE_BEGIN) == 0) {
        co_return std::error_code(static_cast<int>(::GetLastError()), std::system_category());
    }
    std::uint64_t remaining = file.length();
    while (remaining > 0) {
        const auto nextSend = static_cast<DWORD>(std::min<std::uint64_t>(
            remaining, static_cast<std::uint64_t>((std::numeric_limits<DWORD>::max)())));
        if (::TransmitFile(socket.native_handle(), input.get(), nextSend, 0, nullptr, nullptr, 0) !=
            FALSE) {
            remaining -= nextSend;
            continue;
        }
        const auto socketError = ::WSAGetLastError();
        if (socketError == WSAEWOULDBLOCK) {
            const auto waitCompletion = co_await asyncAsio([&socket](auto handler) mutable {
                socket.async_wait(asio::ip::tcp::socket::wait_write, std::move(handler));
            });
            const auto waitError = waitCompletion.errorCode();
            if (waitError) {
                co_return waitError;
            }
            continue;
        }
        co_return std::error_code(socketError, std::system_category());
    }
    co_return std::error_code{};
#else
    co_return co_await writeFileFallback(socket, memory, reusableChunk, file);
#endif
}

}  // namespace ruvia::detail
