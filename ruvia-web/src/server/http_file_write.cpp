#include "server/http_file_write.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <system_error>
#include <utility>

#include "ruvia/core/async.h"

#include "server/response_file_source.h"

#if defined(_WIN32)
#include <mswsock.h>

#include <asio/windows/overlapped_ptr.hpp>
#endif

namespace ruvia::detail {

#if defined(_WIN32)
task<http_response_file_write_result> write_http_response_file(asio::ip::tcp::socket& socket,
    std::string_view head, native_file_type input, http_response_file_view file, connection_scanner::entry& progress) {
    http_response_file_write_result result;
    // A null OVERLAPPED would make every TransmitFile call synchronous until
    // the peer drained the whole range, blocking this worker's event loop
    // behind one slow reader; the overlapped call completes through the
    // socket's IOCP instead and carries its file offset explicitly. Progress
    // is only observable when a call completes, so each call sends at most
    // one read-ahead slice, the same granularity at which the pooled reader
    // renews write_timeout. The head rides in the first call's
    // TRANSMIT_FILE_BUFFERS, saving a send and its completion.
    constexpr std::uint64_t max_transmit_file_bytes = response_file_read_ahead_bytes;
    std::uint64_t offset = file.offset();
    std::uint64_t remaining = file.length();
    while (remaining > 0 || !head.empty()) {
        // A zero byte count would mean "the whole file": a call that only
        // finishes the head passes no file at all. The final byte is sent
        // alone, after the handle was checked once more.
        auto next_send = std::min(remaining, max_transmit_file_bytes);
        if (next_send == remaining && remaining > 1) {
            next_send = remaining - 1;
        }
        TRANSMIT_FILE_BUFFERS buffers{};
        buffers.Head = const_cast<char*>(head.data());
        buffers.HeadLength = static_cast<DWORD>(head.size());
        const auto transmit_completion = co_await ruvia::async_asio<std::size_t>(
            [&socket, &buffers, input, offset, next_send = static_cast<DWORD>(next_send)](auto handler) {
                asio::windows::overlapped_ptr operation(socket.get_executor(), std::move(handler));
                operation.get()->Offset = static_cast<DWORD>(offset & UINT64_C(0xffffffff));
                operation.get()->OffsetHigh = static_cast<DWORD>(offset >> 32);
                const auto transmitted = ::TransmitFile(socket.native_handle(), next_send == 0 ? nullptr : input,
                    next_send, 0, operation.get(), buffers.HeadLength == 0 ? nullptr : &buffers, 0);
                const int socket_error = transmitted != FALSE ? 0 : ::WSAGetLastError();
                if (transmitted != FALSE || socket_error == WSA_IO_PENDING) {
                    static_cast<void>(operation.release());
                    return;
                }
                operation.complete(std::error_code(socket_error, std::system_category()), 0);
            });
        // A failed completion still reports the prefix it transferred, which
        // may cover the head: the response is committed once the head left.
        const auto sent = static_cast<std::uint64_t>(transmit_completion.result());
        const auto head_sent = std::min<std::uint64_t>(sent, head.size());
        const auto file_sent = sent - head_sent;
        head.remove_prefix(static_cast<std::size_t>(head_sent));
        const auto transmit_error = transmit_completion.error_code();
        if (transmit_error || sent == 0 || file_sent > remaining) {
            result.error_ = transmit_error ? transmit_error : std::error_code(asio::error::operation_aborted);
            result.head_written_ = head.empty();
            co_return result;
        }
        offset += file_sent;
        remaining -= file_sent;
        progress.note_write_progress();
        if (remaining != 0) {
            if (const auto changed = validate_native_file(input, file)) {
                result.error_ = changed;
                result.head_written_ = head.empty();
                co_return result;
            }
        }
    }
    result.head_written_ = true;
    co_return result;
}
#endif

}  // namespace ruvia::detail
