#include "server/http_file_write.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <system_error>
#include <utility>

#include "ruvia/core/async.h"

#include "server/http_native_file.h"

#if defined(__linux__)
#include <sys/sendfile.h>

#include <cerrno>
#elif defined(_WIN32)
#include <mswsock.h>

#include <asio/windows/overlapped_ptr.hpp>
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
    // TransmitFile rejects counts above 2,147,483,646 bytes. A null OVERLAPPED
    // would make every call synchronous until the peer drained the whole
    // range, blocking this worker's event loop behind one slow reader; the
    // overlapped call completes through the socket's IOCP instead and carries
    // its file offset explicitly.
    constexpr std::uint64_t max_transmit_file_bytes = UINT64_C(2147483646);
    std::uint64_t offset = file.offset();
    std::uint64_t remaining = file.length();
    while (remaining > 0) {
        const auto next_send = static_cast<DWORD>(std::min(remaining, max_transmit_file_bytes));
        const auto transmit_completion = co_await ruvia::async_asio<std::size_t>(
            [&socket, &input, offset, next_send](auto handler) {
                asio::windows::overlapped_ptr operation(socket.get_executor(), std::move(handler));
                operation.get()->Offset = static_cast<DWORD>(offset & UINT64_C(0xffffffff));
                operation.get()->OffsetHigh = static_cast<DWORD>(offset >> 32);
                const auto transmitted = ::TransmitFile(
                    socket.native_handle(), input.get(), next_send, 0, operation.get(), nullptr, 0);
                const int socket_error = transmitted != FALSE ? 0 : ::WSAGetLastError();
                if (transmitted != FALSE || socket_error == WSA_IO_PENDING) {
                    static_cast<void>(operation.release());
                    return;
                }
                operation.complete(std::error_code(socket_error, std::system_category()), 0);
            });
        if (const auto transmit_error = transmit_completion.error_code()) {
            co_return transmit_error;
        }
        const auto sent = static_cast<std::uint64_t>(transmit_completion.result());
        if (sent == 0 || sent > remaining) {
            co_return asio::error::operation_aborted;
        }
        offset += sent;
        remaining -= sent;
    }
    co_return std::error_code{};
#else
    co_return co_await write_file_fallback(socket, memory, reusable_chunk, file);
#endif
}

}  // namespace ruvia::detail
