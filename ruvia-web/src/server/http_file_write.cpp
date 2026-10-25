#include "server/http_file_write.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <system_error>
#include <utility>

#include "ruvia/core/async.h"

#include "server/http_native_file.h"

#if defined(_WIN32)
#include <mswsock.h>

#include <asio/windows/overlapped_ptr.hpp>
#endif

namespace ruvia::detail {

#if defined(_WIN32)
namespace {

// The handle the response was framed from, or one opened here into `owned`,
// validated against the response's identity snapshot either way: the framing
// handle stays valid across an in-place rewrite, which must fail before any
// byte is sent rather than pair new content with the framed validators.
[[nodiscard]] native_file_type acquire_response_file(
    http_response_file_view file, native_file_handle& owned, std::error_code& ec) noexcept {
    if (const auto* opened = file.opened_handle()) {
        const auto input = borrowed_native_file(*opened);
        ec = validate_native_file(input, file);
        return input;
    }
    owned = open_native_file_for_read(file, ec);
    return owned.get();
}

}  // namespace

std::error_code append_response_file_bytes(http_response_head_buffer& head, http_response_file_view file) {
    if (file.length() > inline_response_file_max_bytes) {
        return std::make_error_code(std::errc::file_too_large);
    }
    native_file_handle owned;
    std::error_code error;
    const auto input = acquire_response_file(file, owned, error);
    if (error || file.length() == 0) {
        return error;
    }
    const auto size = static_cast<std::size_t>(file.length());
    head.append_generated(size, [&error, input, offset = file.offset(), size](char* output) noexcept {
        error = read_native_file_at(input, offset, output, size);
    });
    // A same-size in-place rewrite between the identity check and the read
    // would pair new bytes with the framed validators; nothing has been sent
    // yet, so re-check the handle the bytes came from.
    if (!error) {
        error = validate_native_file(input, file);
    }
    return error;
}

task<http_response_file_write_result> write_http_response_file(asio::ip::tcp::socket& socket,
    std::string_view head, http_response_file_view file, connection_scanner::entry& progress) {
    http_response_file_write_result result;
    native_file_handle owned;
    const auto input = acquire_response_file(file, owned, result.error_);
    if (result.error_) {
        co_return result;
    }

    // A null OVERLAPPED would make every TransmitFile call synchronous until
    // the peer drained the whole range, blocking this worker's event loop
    // behind one slow reader; the overlapped call completes through the
    // socket's IOCP instead and carries its file offset explicitly. Progress
    // is only observable when a call completes, so each call sends a bounded
    // slice (well below TransmitFile's 2,147,483,646-byte limit): write_timeout
    // then measures inactivity over one slice rather than the whole range.
    // The head rides in the first call's TRANSMIT_FILE_BUFFERS, saving a send
    // and its completion.
    constexpr std::uint64_t max_transmit_file_bytes = UINT64_C(256) * 1024;
    std::uint64_t offset = file.offset();
    std::uint64_t remaining = file.length();
    while (remaining > 0 || !head.empty()) {
        // A zero byte count would mean "the whole file": a call that only
        // finishes the head passes no file at all.
        const auto next_send = static_cast<DWORD>(std::min(remaining, max_transmit_file_bytes));
        TRANSMIT_FILE_BUFFERS buffers{};
        buffers.Head = const_cast<char*>(head.data());
        buffers.HeadLength = static_cast<DWORD>(head.size());
        const auto transmit_completion = co_await ruvia::async_asio<std::size_t>(
            [&socket, &buffers, input, offset, next_send](auto handler) {
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
        progress.touch();
    }
    result.head_written_ = true;
    co_return result;
}
#endif

}  // namespace ruvia::detail
