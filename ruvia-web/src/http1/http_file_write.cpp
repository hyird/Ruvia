#include "server/http_file_write.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

#include "ruvia/core/async.h"

#include "server/http_native_file.h"

#if defined(__linux__)
#include <sys/sendfile.h>

#include <cerrno>
#elif defined(_WIN32)
#include <mswsock.h>
#endif

namespace ruvia::detail {

task<std::error_code> write_http_response_file(asio::ip::tcp::socket& socket, worker_memory& memory,
    std::pmr::string* reusable_chunk, http_response_file_view file) {
#if defined(__linux__)
    static_cast<void>(memory);
    static_cast<void>(reusable_chunk);
    std::error_code error;
    auto input = open_native_file_for_read(file, error);
    if (error) {
        co_return error;
    }
    auto offset = static_cast<off_t>(file.offset());
    std::uint64_t remaining = file.length();
    while (remaining > 0) {
        const auto next_send =
            static_cast<std::size_t>(std::min<std::uint64_t>(remaining, 0x7ffff000ULL));
        const auto sent = ::sendfile(socket.native_handle(), input.get(), &offset, next_send);
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
            const auto wait_completion = co_await ruvia::async_asio([&socket](auto handler) mutable {
                socket.async_wait(asio::ip::tcp::socket::wait_write, std::move(handler));
            });
            error = wait_completion.error_code();
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
    static_cast<void>(reusable_chunk);
    std::error_code error;
    auto input = open_native_file_for_read(file, error);
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
        const auto next_send = static_cast<DWORD>(std::min<std::uint64_t>(
            remaining, static_cast<std::uint64_t>((std::numeric_limits<DWORD>::max)())));
        if (::TransmitFile(socket.native_handle(), input.get(), next_send, 0, nullptr, nullptr, 0) !=
            FALSE) {
            remaining -= next_send;
            continue;
        }
        const auto socket_error = ::WSAGetLastError();
        if (socket_error == WSAEWOULDBLOCK) {
            const auto wait_completion = co_await ruvia::async_asio([&socket](auto handler) mutable {
                socket.async_wait(asio::ip::tcp::socket::wait_write, std::move(handler));
            });
            const auto wait_error = wait_completion.error_code();
            if (wait_error) {
                co_return wait_error;
            }
            continue;
        }
        co_return std::error_code(socket_error, std::system_category());
    }
    co_return std::error_code{};
#else
    co_return co_await write_file_fallback(socket, memory, reusable_chunk, file);
#endif
}

}  // namespace ruvia::detail
