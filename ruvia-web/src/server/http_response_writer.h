#pragma once

#include <array>
#include <cstddef>
#include <string_view>
#include <system_error>
#include <type_traits>

#if defined(__linux__)
#include <sys/ioctl.h>
#include <sys/socket.h>
#endif

#include <asio.hpp>

#include "ruvia/core/async.h"
#include "ruvia/core/task.h"
#include "ruvia/http/http1_response_head_plan.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_response_server.h"

#include "server/connection_scanner.h"
#include "server/http_file_write.h"
#include "server/response_file_source.h"

namespace ruvia::detail {

// A write commits the final status only once the entire head is out; it
// completes only when it also ends without error.
struct http1_buffered_response_write_result final {
    std::optional<http_status_code> committed_status_;
    bool completed_{false};
};

// `bytes_transferred` is the composed async-write prefix accepted before `error`.
[[nodiscard]] inline http1_buffered_response_write_result classify_http1_buffered_response_write(
    const http1_buffered_response_plan& plan, std::size_t response_head_bytes, std::error_code error,
    std::size_t bytes_transferred) noexcept {
    if (response_head_bytes == 0 || bytes_transferred < response_head_bytes) {
        return {};
    }
    return {plan.response_status(), !error};
}
// Only for a final buffered response followed immediately by send shutdown.
// Linux can carry the response tail and FIN together; other platforms keep
// their ordinary send flags. Reusable and streaming writes must not use it.
#if defined(__linux__)
inline constexpr asio::socket_base::message_flags final_plain_tcp_send_flags = MSG_MORE;
#else
inline constexpr asio::socket_base::message_flags final_plain_tcp_send_flags = 0;
#endif

// asio::async_write completion condition that records each partial transfer on
// the connection's scanner entry, so write_timeout measures inactivity rather
// than the time to drain the whole response.
class http_write_progress final {
public:
    explicit http_write_progress(connection_scanner::entry& entry) noexcept
        : entry_(&entry) {}

    [[nodiscard]] std::size_t operator()(const std::error_code& error, std::size_t bytes_transferred) const noexcept {
        entry_->touch();
        return asio::transfer_all()(error, bytes_transferred);
    }

private:
    connection_scanner::entry* entry_;
};

// Puts an accepted plain TCP socket into user non-blocking mode once, before
// its first I/O, so try_plain_tcp_sync_write() can offer bytes without
// blocking the worker. Asio's own internal non-blocking state is not enough:
// write_some() polls indefinitely unless the user mode is set. Asynchronous
// operations are unaffected on every platform: the reactor already runs the
// descriptor non-blocking, and on Windows asio issues overlapped
// WSASend/WSARecv/TransmitFile, which ignore FIONBIO, while a synchronous
// send() on a full send buffer returns WSAEWOULDBLOCK (asio::error::would_block)
// instead of blocking. A socket left blocking (the call failed) keeps every
// write on the asynchronous path.
inline void enable_plain_tcp_sync_write(asio::ip::tcp::socket& socket) noexcept {
    std::error_code ignored;
#if defined(__linux__)
    // This socket is freshly accepted TCP, not an external descriptor. The
    // public command sets Asio's user mode as well as the kernel flag, without
    // the assigned-descriptor F_GETFL/F_SETFL round trip.
    struct non_blocking_command final {
        int enabled_{1};
        [[nodiscard]] int name() const noexcept {
            return FIONBIO;
        }
        [[nodiscard]] int* data() noexcept {
            return &enabled_;
        }
    } command;
    socket.io_control(command, ignored);
#else
    socket.non_blocking(true, ignored);
#endif
}

// Try one non-blocking plain TCP write before suspending. Returns true when
// the write completed or hit a hard error (outcome in ec/bytes_written), and
// false when the remainder must go through async_write.
template <typename const_buffer_sequence_type>
[[nodiscard]] inline bool try_plain_tcp_sync_write(asio::ip::tcp::socket& socket,
    const const_buffer_sequence_type& buffers, std::size_t total_bytes,
    asio::socket_base::message_flags flags, std::error_code& ec,
    std::size_t& bytes_written) noexcept {
    ec.clear();
    bytes_written = 0;
    if (!socket.non_blocking()) {
        return false;
    }
    bytes_written = socket.send(buffers, flags, ec);
    if (!ec) {
        return bytes_written == total_bytes;
    }
    if (ec == asio::error::would_block || ec == asio::error::try_again ||
        ec == asio::error::interrupted) {
        ec.clear();
        return false;
    }
    return true;
}

#if defined(_WIN32)
// Windows keeps its overlapped native transfer for committed file ranges.
inline task<std::error_code> write_http1_native_file_body(asio::ip::tcp::socket& stream,
    const http_response& response, connection_scanner::entry& progress) {
    for (std::size_t index = 0; index < response.body_segment_count(); ++index) {
        const auto segment = response.body_segment(index);
        if (segment.file_) {
            if (const auto written = co_await write_http_response_file(stream, {}, *segment.file_, progress);
                written.error_) {
                co_return written.error_;
            }
        } else if (!segment.bytes_.empty()) {
            auto part = co_await ruvia::async_asio<std::size_t>(
                [&stream, &progress, bytes = segment.bytes_](auto handler) mutable {
                    asio::async_write(
                        stream, asio::buffer(bytes), http_write_progress(progress), std::move(handler));
                });
            if (const auto error = part.error_code()) {
                co_return error;
            }
        }
    }
    co_return std::error_code{};
}
#endif

// A buffered response serialized into the connection's output.
struct http1_response_write_start final {
    // Output offset at which this response's head is complete. Responses held
    // back before it precede it; without them this is the head's length.
    std::size_t head_bytes_{};
    // In-memory body bytes not coalesced into the output.
    std::string_view body_;
    // The response stays in the output for a later coalesced write.
    bool held_{false};
};

// Appends the response head to `output`, after any pipelined responses held
// back there, and coalesces an in-memory body that fits the inline head
// storage. A nonzero `hold_limit` means the connection already holds its next
// request: the body is then coalesced up to that many output bytes, and a
// response wholly in the output below the limit is held back instead of
// written. File bodies are never coalesced or held; the caller writes any
// held-back output first, then continues in finish_file_response_write().
[[nodiscard]] inline http1_response_write_start start_response_write(http_response_head_buffer& output,
    const http_response& response, const http1_buffered_response_plan& response_plan, std::size_t hold_limit) {
    append_http1_response_head(response, output, response_plan.head_plan());
    http1_response_write_start start;
    start.head_bytes_ = output.view().size();
    if (response.file_body() || response.has_multipart_file_body()) {
        return start;
    }

    auto body = response_plan.send_body() ? response.body_bytes() : std::string_view{};
    if (!body.empty() && (output.can_append_on_stack(body.size()) ||
                             (start.head_bytes_ < hold_limit && body.size() < hold_limit - start.head_bytes_))) {
        output.append(body);
        body = {};
    }
    start.body_ = body;
    start.held_ = body.empty() && output.view().size() < hold_limit;
    return start;
}

// Writes a file or multipart/byteranges response whose head start_response_write()
// serialized into an otherwise empty `head`. `progress` is the connection's
// scanner entry; every partial transfer touches it. A plain-TCP whole-file body
// may append its bytes to `head` so that head and body leave in one send.
template <typename stream_type>
task<http1_buffered_response_write_result> finish_file_response_write(stream_type& stream,
    http_response_head_buffer& head, const http_response& response,
    const http1_buffered_response_plan& response_plan, const http1_response_write_start& start,
    const response_file_io& file_io, connection_scanner::entry& progress) {
    const auto response_head_bytes = start.head_bytes_;
#if defined(_WIN32)
    if constexpr (std::is_same_v<std::remove_cvref_t<stream_type>, asio::ip::tcp::socket> &&
                  native_response_file_write_available) {
        if (response_plan.send_body() && !response.has_multipart_file_body()) {
            // The file is opened (or its framing handle borrowed) and, when
            // small, read before any byte is sent: a file that is missing or
            // changed fails before the head commits a status.
            const auto file = *response.file_body();
            if (file.length() > inline_response_file_max_bytes) {
                const auto written = co_await write_http_response_file(stream, head.view(), file, progress);
                co_return classify_http1_buffered_response_write(response_plan, response_head_bytes,
                    written.error_, written.head_written_ ? response_head_bytes : 0);
            }
            if (const auto read_error = append_response_file_bytes(head, file)) {
                co_return classify_http1_buffered_response_write(
                    response_plan, response_head_bytes, read_error, 0);
            }
            const auto bytes = head.view();
            std::error_code write_ec;
            std::size_t written_bytes = 0;
            if (!try_plain_tcp_sync_write(stream, asio::buffer(bytes), bytes.size(), 0, write_ec, written_bytes)) {
                auto write_completion = co_await ruvia::async_asio<std::size_t>(
                    [&stream, &progress, rest = bytes.substr(written_bytes)](auto handler) mutable {
                        asio::async_write(
                            stream, asio::buffer(rest), http_write_progress(progress), std::move(handler));
                    });
                write_ec = write_completion.error_code();
                written_bytes += write_completion.result();
            }
            co_return classify_http1_buffered_response_write(
                response_plan, response_head_bytes, write_ec, written_bytes);
        }
        auto write_completion = co_await ruvia::async_asio<std::size_t>(
            [&stream, &progress, head_view = head.view()](auto handler) mutable {
                asio::async_write(
                    stream, asio::buffer(head_view), http_write_progress(progress), std::move(handler));
            });
        const auto ec = write_completion.error_code();
        const auto bytes_transferred = write_completion.result();
        if (ec || !response_plan.send_body()) {
            co_return classify_http1_buffered_response_write(
                response_plan, response_head_bytes, ec, bytes_transferred);
        }
        const auto body_error = co_await write_http1_native_file_body(stream, response, progress);
        co_return classify_http1_buffered_response_write(
            response_plan, response_head_bytes, body_error, response_head_bytes);
    }
#endif
    response_file_source source(response, file_io);
    auto first = response_body_chunk::end();
#if defined(__linux__)
    if (response_plan.send_body()) {
        // 先验证并读取首块，再提交响应头；首块和头共用一次散集写。
        first = co_await source.next();
        if (first.kind() == response_body_chunk::kind_type::failed) {
            co_return classify_http1_buffered_response_write(response_plan, response_head_bytes,
                std::make_error_code(std::errc::io_error), 0);
        }
    }
#endif
    const std::array<asio::const_buffer, 2> buffers{asio::buffer(head.view()), asio::buffer(first.bytes())};
    auto write_completion = co_await ruvia::async_asio<std::size_t>(
        [&stream, &progress, &buffers](auto handler) mutable {
            asio::async_write(stream, buffers, http_write_progress(progress), std::move(handler));
        });
    if (write_completion.error_code() || !response_plan.send_body()) {
        co_return classify_http1_buffered_response_write(response_plan, response_head_bytes,
            write_completion.error_code(), write_completion.result());
    }
#if defined(__linux__)
    if (first.kind() == response_body_chunk::kind_type::end) {
        co_return classify_http1_buffered_response_write(response_plan, response_head_bytes, {}, response_head_bytes);
    }
#endif
    for (;;) {
        const auto chunk = co_await source.next();
        if (chunk.kind() == response_body_chunk::kind_type::end) {
            co_return classify_http1_buffered_response_write(response_plan, response_head_bytes, {}, response_head_bytes);
        }
        if (chunk.kind() == response_body_chunk::kind_type::failed) {
            co_return classify_http1_buffered_response_write(response_plan, response_head_bytes,
                std::make_error_code(std::errc::io_error), response_head_bytes);
        }
        auto written = co_await ruvia::async_asio<std::size_t>(
            [&stream, &progress, bytes = chunk.bytes()](auto handler) mutable {
                asio::async_write(stream, asio::buffer(bytes), http_write_progress(progress), std::move(handler));
            });
        if (written.error_code()) {
            co_return classify_http1_buffered_response_write(response_plan, response_head_bytes,
                written.error_code(), response_head_bytes);
        }
    }
}

}  // namespace ruvia::detail
