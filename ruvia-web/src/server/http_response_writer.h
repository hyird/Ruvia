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
#include "ruvia/http/http_response_head_buffer.h"
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
// held-back output first, then continues with http1_file_response_body.
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

// The body of a file or multipart/byteranges response. start() runs before
// any byte of the head leaves: it reads the first chunk, which validates the
// first file range, or for a kernel transfer validates the response's
// handle. A missing, unreadable or changed file can then still be answered
// with an error response. finish() writes the head start_response_write()
// serialized into an otherwise empty `head`, with the first chunk in the same
// send, then the rest of the body. Every partial transfer touches `progress`.
template <typename stream_type>
class http1_file_response_body final {
public:
    // `response` is borrowed until finish() completes.
    http1_file_response_body(const http_response& response, const response_file_io& file_io)
        : source_(response, file_io)
#if defined(_WIN32)
          ,
          transmit_file_(transmit_candidate(response))
#endif
    {
    }

    http1_file_response_body(const http1_file_response_body&) = delete;
    http1_file_response_body& operator=(const http1_file_response_body&) = delete;
    http1_file_response_body(http1_file_response_body&&) = delete;
    http1_file_response_body& operator=(http1_file_response_body&&) = delete;

    // False when the body failed with nothing sent; failure() says why.
    [[nodiscard]] task<bool> start(const http1_buffered_response_plan& response_plan) {
        // HEAD and status-suppressed bodies transfer no representation.
        if (response_plan.body_plan().body_suppressed()) {
            co_return true;
        }
#if defined(_WIN32)
        if (transmit_file_) {
            // The framing handle stays valid across an in-place rewrite, which
            // must fail before any byte is sent.
            if (const auto error =
                    validate_native_file(borrowed_native_file(*transmit_file_->opened_handle()), *transmit_file_)) {
                transmit_failure_.emplace(response_file_failure::read_failed(error));
                co_return false;
            }
            transmit_ = true;
            co_return true;
        }
#endif
        first_ = co_await source_.next();
        co_return first_.kind() != response_body_chunk::kind_type::failed;
    }

    [[nodiscard]] task<http1_buffered_response_write_result> finish(stream_type& stream,
        http_response_head_buffer& head, const http1_buffered_response_plan& response_plan,
        const http1_response_write_start& start, connection_scanner::entry& progress) {
        const auto response_head_bytes = start.head_bytes_;
#if defined(_WIN32)
        if constexpr (std::is_same_v<std::remove_cvref_t<stream_type>, asio::ip::tcp::socket>) {
            if (transmit_) {
                const auto written = co_await write_http_response_file(stream, head.view(),
                    borrowed_native_file(*transmit_file_->opened_handle()), *transmit_file_, progress);
                co_return classify_http1_buffered_response_write(response_plan, response_head_bytes,
                    written.error_, written.head_written_ ? response_head_bytes : 0);
            }
        }
#endif
        const auto first_written = co_await connection_gather_write(stream, head.view(), first_.bytes(), progress);
        if (first_written.error_ || !response_plan.send_body()) {
            co_return classify_http1_buffered_response_write(
                response_plan, response_head_bytes, first_written.error_, first_written.bytes_);
        }
        if (first_.kind() == response_body_chunk::kind_type::end) {
            co_return classify_http1_buffered_response_write(response_plan, response_head_bytes, {}, response_head_bytes);
        }
        for (;;) {
            const auto chunk = co_await source_.next();
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

    // Why start() or finish() could not produce the body from the file.
    [[nodiscard]] const response_file_failure* failure() const noexcept {
        if (const auto* failure = source_.failure()) {
            return failure;
        }
#if defined(_WIN32)
        if (transmit_failure_) {
            return &*transmit_failure_;
        }
#endif
        return nullptr;
    }

private:
#if defined(_WIN32)
    // The single range a kernel transfer may send from the response's handle.
    [[nodiscard]] static std::optional<http_response_file_view> transmit_candidate(
        [[maybe_unused]] const http_response& response) noexcept {
        if constexpr (std::is_same_v<std::remove_cvref_t<stream_type>, asio::ip::tcp::socket>) {
            if (!response.has_multipart_file_body()) {
                const auto file = response.file_body();
                if (file && file->length() > transmit_file_min_bytes && file->opened_handle() != nullptr) {
                    return file;
                }
            }
        }
        return std::nullopt;
    }
#endif

    response_file_source source_;
    response_body_chunk first_{response_body_chunk::end()};
#if defined(_WIN32)
    std::optional<http_response_file_view> transmit_file_;
    std::optional<response_file_failure> transmit_failure_;
    bool transmit_{false};
#endif
};

}  // namespace ruvia::detail
