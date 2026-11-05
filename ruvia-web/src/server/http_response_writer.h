#pragma once

#include <array>
#include <coroutine>
#include <cstddef>
#include <initializer_list>
#include <new>
#include <optional>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

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
    return {plan.body_plan().response_status(), !error};
}
// Only for a final buffered response followed immediately by send shutdown.
// Linux can carry the response tail and FIN together; other platforms keep
// their ordinary send flags. Reusable and streaming writes must not use it.
#if defined(__linux__)
inline constexpr asio::socket_base::message_flags final_plain_tcp_send_flags = MSG_MORE;
#else
inline constexpr asio::socket_base::message_flags final_plain_tcp_send_flags = 0;
#endif

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

// Appends `parts` to `output` so a TLS stream encrypts them as one record. A
// gather write of the same parts is equivalent on the wire, so failing to
// allocate only selects that fallback: false leaves `output` unchanged. The
// reservation is the only allocation; the appends then fit.
[[nodiscard]] inline bool try_coalesce_tls_frame(
    http_response_head_buffer& output, std::initializer_list<std::string_view> parts) noexcept {
    std::size_t total = 0;
    for (const auto part : parts) {
        total += part.size();
    }
    try {
        output.reserve_additional(total);
    } catch (const std::bad_alloc&) {
        return false;
    }
    for (const auto part : parts) {
        output.append(part);
    }
    return true;
}

// A write either accepts every byte or ends with an error.
struct connection_write_outcome final {
    std::error_code error_;
    // Prefix of the written bytes the transport accepted.
    std::size_t bytes_{};
};

// Initiates the async_write of what follows the first `written` bytes of
// `head` then `body`. Every partial transfer touches `progress`.
template <typename stream_type, typename handler_type>
void async_write_after(stream_type& stream, std::string_view head, std::string_view body, std::size_t written,
    connection_scanner::entry& progress, handler_type&& handler) {
    if (written >= head.size()) {
        asio::async_write(stream, asio::buffer(body.substr(written - head.size())), connection_write_progress(progress),
            std::forward<handler_type>(handler));
        return;
    }
    const std::array<asio::const_buffer, 2> remaining{asio::buffer(head.substr(written)), asio::buffer(body)};
    asio::async_write(stream, remaining, connection_write_progress(progress), std::forward<handler_type>(handler));
}

// Awaitable write of `head` then `body`. Plain TCP first offers both to one
// non-blocking send; only a remainder suspends, as an async_write awaited in
// place, so a write the socket accepts at once costs no coroutine frame.
// Every transfer, synchronous or not, touches `progress`. The views must stay
// valid until the co_await completes.
template <typename stream_type>
class connection_gather_write final {
public:
    connection_gather_write(stream_type& stream, std::string_view head, std::string_view body,
        connection_scanner::entry& progress) noexcept
        : stream_(&stream),
          head_(head),
          body_(body),
          progress_(&progress) {}

    connection_gather_write(const connection_gather_write&) = delete;
    connection_gather_write& operator=(const connection_gather_write&) = delete;

    [[nodiscard]] bool await_ready() noexcept {
        if constexpr (std::is_same_v<std::remove_cvref_t<stream_type>, asio::ip::tcp::socket>) {
            const std::array<asio::const_buffer, 2> buffers{asio::buffer(head_), asio::buffer(body_)};
            const bool settled = try_plain_tcp_sync_write(
                *stream_, buffers, head_.size() + body_.size(), 0, written_.error_, written_.bytes_);
            if (written_.bytes_ != 0) {
                progress_->note_write_progress();
            }
            return settled;
        } else {
            return false;
        }
    }

    [[nodiscard]] bool await_suspend(std::coroutine_handle<> handle) {
        return remainder_.emplace(remainder_initiation{this}).await_suspend(handle);
    }

    [[nodiscard]] connection_write_outcome await_resume() {
        if (remainder_) {
            const auto completion = remainder_->await_resume();
            written_.error_ = completion.error_code();
            written_.bytes_ += completion.result();
        }
        return written_;
    }

private:
    struct remainder_initiation final {
        connection_gather_write* owner_;

        template <typename handler_type>
        void operator()(handler_type&& handler) const {
            async_write_after(*owner_->stream_, owner_->head_, owner_->body_, owner_->written_.bytes_,
                *owner_->progress_, std::forward<handler_type>(handler));
        }
    };

    stream_type* stream_;
    std::string_view head_;
    std::string_view body_;
    connection_scanner::entry* progress_;
    connection_write_outcome written_;
    std::optional<asio_completion_awaiter<std::size_t, remainder_initiation>> remainder_;
};

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
            if (const auto written = co_await connection_gather_write(stream, segment.bytes_, {}, progress); written.error_) {
                co_return written.error_;
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
    const http_response& response, const http1_buffered_response_plan& response_plan, std::size_t hold_limit,
    bool coalesce_for_tls = false) {
    append_http1_response_head(response, output, response_plan.head_plan());
    http1_response_write_start start;
    start.head_bytes_ = output.view().size();
    if (response.file_body() || response.has_multipart_file_body()) {
        return start;
    }

    auto body = response_plan.send_body() ? response.body_bytes() : std::string_view{};
    if (!body.empty()) {
        const bool can_hold = start.head_bytes_ < hold_limit && body.size() < hold_limit - start.head_bytes_;
        if (can_hold || (coalesce_for_tls && output.can_append_on_stack(body.size()))) {
            output.append(body);
            body = {};
        } else if (coalesce_for_tls && start.head_bytes_ <= response_head_retained_heap_bytes &&
                   body.size() <= response_head_retained_heap_bytes - start.head_bytes_ &&
                   try_coalesce_tls_frame(output, {body})) {
            body = {};
        }
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
        if (!response_plan.body_plan().body_suppressed() && !response.has_multipart_file_body()) {
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
            const auto written = co_await connection_gather_write(stream, head.view(), {}, progress);
            co_return classify_http1_buffered_response_write(
                response_plan, response_head_bytes, written.error_, written.bytes_);
        }
        const auto head_written = co_await connection_gather_write(stream, head.view(), {}, progress);
        if (head_written.error_ || !response_plan.send_body()) {
            co_return classify_http1_buffered_response_write(
                response_plan, response_head_bytes, head_written.error_, head_written.bytes_);
        }
        const auto body_error = co_await write_http1_native_file_body(stream, response, progress);
        co_return classify_http1_buffered_response_write(
            response_plan, response_head_bytes, body_error, response_head_bytes);
    }
#endif
    response_file_source source(response, file_io);
    auto first = response_body_chunk::end();
    if (!response_plan.body_plan().body_suppressed()) {
        // Validate the first file range before committing the head, including
        // empty files for which send_body() correctly reports no payload.
        first = co_await source.next();
        if (first.kind() == response_body_chunk::kind_type::failed) {
            co_return classify_http1_buffered_response_write(response_plan, response_head_bytes,
                std::make_error_code(std::errc::io_error), 0);
        }
    }
    const auto first_written = co_await connection_gather_write(stream, head.view(), first.bytes(), progress);
    if (first_written.error_ || !response_plan.send_body()) {
        co_return classify_http1_buffered_response_write(
            response_plan, response_head_bytes, first_written.error_, first_written.bytes_);
    }
    if (first.kind() == response_body_chunk::kind_type::end) {
        co_return classify_http1_buffered_response_write(response_plan, response_head_bytes, {}, response_head_bytes);
    }
    for (;;) {
        const auto chunk = co_await source.next();
        if (chunk.kind() == response_body_chunk::kind_type::end) {
            co_return classify_http1_buffered_response_write(response_plan, response_head_bytes, {}, response_head_bytes);
        }
        if (chunk.kind() == response_body_chunk::kind_type::failed) {
            co_return classify_http1_buffered_response_write(response_plan, response_head_bytes,
                std::make_error_code(std::errc::io_error), response_head_bytes);
        }
        if (const auto written = co_await connection_gather_write(stream, chunk.bytes(), {}, progress); written.error_) {
            co_return classify_http1_buffered_response_write(
                response_plan, response_head_bytes, written.error_, response_head_bytes);
        }
    }
}

}  // namespace ruvia::detail
