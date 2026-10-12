#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>
#include <system_error>
#include <type_traits>

#include <asio.hpp>

#include "ruvia/core/async.h"
#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/core/task.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_response_server.h"

#include "server/connection_scanner.h"
#include "server/http1_buffered_response_write.h"
#include "server/http_file_write.h"
#include "server/response_file_source.h"

namespace ruvia::detail {

// asio::async_write completion condition that records each partial transfer on
// the connection's scanner entry, so write_timeout measures inactivity rather
// than the time to drain the whole response.
class http_write_progress final {
public:
    explicit http_write_progress(connection_scanner::entry_type& entry) noexcept
        : entry_(&entry) {}

    [[nodiscard]] std::size_t operator()(const std::error_code& error, std::size_t bytes_transferred) const noexcept {
        entry_->touch();
        return asio::transfer_all()(error, bytes_transferred);
    }

private:
    connection_scanner::entry_type* entry_;
};

// Try one non-blocking plain TCP write before suspending. Returns true when
// the write completed or hit a hard error (outcome in ec/bytes_written), and
// false when the remainder must go through async_write.
template <typename const_buffer_sequence_type>
[[nodiscard]] inline bool try_plain_tcp_sync_write(asio::ip::tcp::socket& socket,
    const const_buffer_sequence_type& buffers, std::size_t total_bytes, std::error_code& ec,
    std::size_t& bytes_written) noexcept {
#ifdef _WIN32
    // Windows async sockets use overlapped I/O without making synchronous
    // send calls non-blocking. A write_some() here can therefore block the
    // worker indefinitely when the peer's receive window is exhausted.
    // Keep all writes on the overlapped async path.
    (void)socket;
    (void)buffers;
    (void)total_bytes;
    ec.clear();
    bytes_written = 0;
    return false;
#else
    ec.clear();
    // Asio's internal native non-blocking mode is not enough: write_some()
    // still polls indefinitely unless its user non-blocking mode is enabled.
    if (!socket.non_blocking()) {
        socket.non_blocking(true, ec);
        if (ec) {
            bytes_written = 0;
            return true;
        }
    }
    bytes_written = socket.write_some(buffers, ec);
    if (!ec) {
        return bytes_written == total_bytes;
    }
    if (ec == asio::error::would_block || ec == asio::error::try_again ||
        ec == asio::error::interrupted) {
        ec.clear();
        return false;
    }
    return true;
#endif
}

// Writes a committed file or multipart/byteranges body. Plain TCP hands file
// ranges to the kernel (sendfile/TransmitFile) where the platform has one;
// TLS, or TCP without a native path, reads through response_file_source.
template <typename stream_type>
task<std::error_code> write_http1_file_body(stream_type& stream, const http_response& response,
    const response_file_io& file_io, connection_scanner::entry_type& progress) {
    if constexpr (std::is_same_v<std::remove_cvref_t<stream_type>, asio::ip::tcp::socket> &&
                  native_response_file_write_available) {
        for (std::size_t index = 0; index < response.body_segment_count(); ++index) {
            const auto segment = response.body_segment(index);
            if (segment.file_) {
                if (const auto error = co_await write_http_response_file(stream, *segment.file_, progress)) {
                    co_return error;
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
    } else {
        response_file_source source(response, file_io);
        for (;;) {
            const auto chunk = co_await source.next();
            if (chunk.kind() == response_body_chunk::kind_type::end) {
                co_return std::error_code{};
            }
            if (chunk.kind() == response_body_chunk::kind_type::failed) {
                co_return std::make_error_code(std::errc::io_error);
            }
            auto written = co_await ruvia::async_asio<std::size_t>(
                [&stream, &progress, bytes = chunk.bytes()](auto handler) mutable {
                    asio::async_write(
                        stream, asio::buffer(bytes), http_write_progress(progress), std::move(handler));
                });
            if (const auto error = written.error_code()) {
                co_return error;
            }
        }
    }
}

// The synchronous start of a buffered response write. finished_ is set when
// that start already settled the outcome; otherwise finish_response_write()
// sends the remainder.
struct http1_response_write_start final {
    std::optional<http1_buffered_response_write_result> finished_;
    // Serialized head length, excluding any small body coalesced into it.
    std::size_t head_bytes_{};
    // In-memory body bytes not coalesced into the head.
    std::string_view body_;
    // Prefix of head-then-body already accepted by the socket.
    std::size_t written_bytes_{};
};

// Serializes the head, coalesces a small in-memory body into it and, on plain
// TCP, offers the bytes to one non-blocking write. A response the socket
// accepts at once therefore completes without a coroutine frame. File bodies
// always continue in finish_response_write().
template <typename stream_type>
[[nodiscard]] http1_response_write_start start_response_write(stream_type& stream,
    http_response_head_buffer& head, const http_response& response, const http1_buffered_response_plan& response_plan) {
    head.reset();
    append_http1_response_head(response, head, response_plan.head_plan());
    http1_response_write_start start;
    start.head_bytes_ = head.view().size();
    if (response.file_body() || response.has_multipart_file_body()) {
        return start;
    }

    auto body = response_plan.send_body() ? response.body_bytes() : std::string_view{};
    if (!body.empty() && head.can_append_on_stack(body.size())) {
        head.append(body);
        body = {};
    }
    start.body_ = body;
    if constexpr (std::is_same_v<std::remove_cvref_t<stream_type>, asio::ip::tcp::socket>) {
        const auto head_view = head.view();
        std::error_code write_ec;
        const bool write_done = body.empty()
                                    ? try_plain_tcp_sync_write(stream, asio::buffer(head_view), head_view.size(),
                                          write_ec, start.written_bytes_)
                                    : try_plain_tcp_sync_write(stream,
                                          std::array<asio::const_buffer, 2>{asio::buffer(head_view), asio::buffer(body)},
                                          head_view.size() + body.size(), write_ec, start.written_bytes_);
        if (write_done) {
            start.finished_ = classify_http1_buffered_response_write(
                response_plan, start.head_bytes_, write_ec, start.written_bytes_);
        }
    }
    return start;
}

// Completes a write whose start did not finish, with the arguments given to
// start_response_write(). `progress` is the connection's scanner entry; every
// partial transfer touches it.
template <typename stream_type>
task<http1_buffered_response_write_result> finish_response_write(stream_type& stream,
    const http_response_head_buffer& head, const http_response& response,
    const http1_buffered_response_plan& response_plan, const http1_response_write_start& start,
    const response_file_io& file_io, connection_scanner::entry_type& progress) {
    const auto response_head_bytes = start.head_bytes_;
    if (response.file_body() || response.has_multipart_file_body()) {
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
        const auto body_error = co_await write_http1_file_body(stream, response, file_io, progress);
        co_return classify_http1_buffered_response_write(
            response_plan, response_head_bytes, body_error, response_head_bytes);
    }

    const auto head_view = head.view();
    const auto body = start.body_;
    const auto written_bytes = start.written_bytes_;
    const std::array<asio::const_buffer, 2> remaining =
        written_bytes < head_view.size()
            ? std::array<asio::const_buffer, 2>{asio::buffer(head_view.substr(written_bytes)), asio::buffer(body)}
            : std::array<asio::const_buffer, 2>{
                  asio::buffer(body.substr(written_bytes - head_view.size())), asio::const_buffer{}};
    auto write_completion = co_await ruvia::async_asio<std::size_t>(
        [&stream, &progress, &remaining](auto handler) mutable {
            asio::async_write(stream, remaining, http_write_progress(progress), std::move(handler));
        });
    co_return classify_http1_buffered_response_write(response_plan, response_head_bytes,
        write_completion.error_code(), written_bytes + write_completion.result());
}

}  // namespace ruvia::detail
