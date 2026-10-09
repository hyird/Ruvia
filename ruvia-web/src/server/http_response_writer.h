#pragma once

#include <array>
#include <cstddef>
#include <string_view>
#include <system_error>
#include <type_traits>

#include <asio.hpp>

#include "ruvia/core/async.h"
#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/core/task.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_response_server.h"

#include "server/http1_buffered_response_write.h"
#include "server/http_file_write.h"

namespace ruvia::detail {

// Optimistic synchronous send for plain TCP. The socket is already
// non-blocking (the session always reads asynchronously before it writes), so
// write_some issues the same single send syscall async_write would -- but a
// full write completes without suspending or re-entering the reactor, which
// lets the session release its work set in the same tick instead of holding
// it across a queued completion. Under high connection counts that collapses
// the peak number of live work sets. Returns true when the attempt finished
// the write or hit a hard error (outcome in ec/bytes_written); false when the
// remainder must go through async_write (would_block or a partial write).
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

template <typename stream_type>
task<http1_buffered_response_write_result> write_response_with_scratch(stream_type& stream,
    worker_memory& memory, http_response_head_buffer& head, std::pmr::string* file_chunk_buffer,
    const http_response& response, const http1_buffered_response_plan& response_plan) {
    head.reset();
    append_http1_response_head(response, head, response_plan.head_plan());
    const auto response_head_bytes = head.view().size();
    if (response.has_multipart_file_body()) {
        auto write_completion = co_await ruvia::async_asio<std::size_t>(
            [&stream, head_view = head.view()](auto handler) mutable {
                asio::async_write(stream, asio::buffer(head_view), std::move(handler));
            });
        auto error = write_completion.error_code();
        auto written = write_completion.result();
        if (error || !response_plan.send_body()) {
            co_return classify_http1_buffered_response_write(response_plan, response_head_bytes, error, written);
        }
        for (std::size_t index = 0; index < response.body_segment_count(); ++index) {
            const auto segment = response.body_segment(index);
            if (segment.file_) {
                error = co_await write_http_response_file(stream, memory, file_chunk_buffer, *segment.file_);
                if (error) {
                    break;
                }
            } else if (!segment.bytes_.empty()) {
                auto part = co_await ruvia::async_asio<std::size_t>(
                    [&stream, bytes = segment.bytes_](auto handler) mutable {
                        asio::async_write(stream, asio::buffer(bytes), std::move(handler));
                    });
                error = part.error_code();
                if (error) {
                    break;
                }
            }
        }
        co_return classify_http1_buffered_response_write(
            response_plan, response_head_bytes, error, response_head_bytes);
    }
    if (const auto file_body = response.file_body()) {
        auto write_completion = co_await ruvia::async_asio<std::size_t>(
            [&stream, head_view = head.view()](auto handler) mutable {
                asio::async_write(stream, asio::buffer(head_view), std::move(handler));
            });
        const auto ec = write_completion.error_code();
        const auto bytes_transferred = write_completion.result();
        if (ec) {
            co_return classify_http1_buffered_response_write(
                response_plan, response_head_bytes, ec, bytes_transferred);
        }
        if (!response_plan.send_body()) {
            co_return classify_http1_buffered_response_write(
                response_plan, response_head_bytes, {}, bytes_transferred);
        }

        const auto file_error =
            co_await write_http_response_file(stream, memory, file_chunk_buffer, *file_body);
        co_return classify_http1_buffered_response_write(
            response_plan, response_head_bytes, file_error, response_head_bytes);
    }

    constexpr bool plain_tcp_stream =
        std::is_same_v<std::remove_cvref_t<stream_type>, asio::ip::tcp::socket>;
    auto body = response_plan.send_body() ? response.body_bytes() : std::string_view{};
    if (!body.empty() && head.can_append_on_stack(body.size())) {
        head.append(body);
        body = {};
    }
    if (body.empty()) {
        const auto head_view = head.view();
        std::error_code write_ec;
        std::size_t written_bytes = 0;
        bool write_done = false;
        if constexpr (plain_tcp_stream) {
            write_done = try_plain_tcp_sync_write(
                stream, asio::buffer(head_view), head_view.size(), write_ec, written_bytes);
        }
        if (!write_done) {
            auto write_completion = co_await ruvia::async_asio<std::size_t>(
                [&stream, remaining = head_view.substr(written_bytes)](auto handler) mutable {
                    asio::async_write(stream, asio::buffer(remaining), std::move(handler));
                });
            write_ec = write_completion.error_code();
            written_bytes += write_completion.result();
        }
        co_return classify_http1_buffered_response_write(
            response_plan, response_head_bytes, write_ec, written_bytes);
    }
    const auto head_view = head.view();
    const std::array<asio::const_buffer, 2> buffers{asio::buffer(head_view), asio::buffer(body)};
    const auto total_bytes = head_view.size() + body.size();
    std::error_code write_ec;
    std::size_t written_bytes = 0;
    bool write_done = false;
    if constexpr (plain_tcp_stream) {
        write_done = try_plain_tcp_sync_write(stream, buffers, total_bytes, write_ec, written_bytes);
    }
    if (!write_done) {
        const std::array<asio::const_buffer, 2> remaining =
            written_bytes < head_view.size()
                ? std::array<asio::const_buffer, 2>{asio::buffer(head_view.substr(written_bytes)),
                      asio::buffer(body)}
                : std::array<asio::const_buffer, 2>{
                      asio::buffer(body.substr(written_bytes - head_view.size())),
                      asio::const_buffer{}};
        auto write_completion =
            co_await ruvia::async_asio<std::size_t>([&stream, &remaining](auto handler) mutable {
                asio::async_write(stream, remaining, std::move(handler));
            });
        write_ec = write_completion.error_code();
        written_bytes += write_completion.result();
    }
    co_return classify_http1_buffered_response_write(
        response_plan, response_head_bytes, write_ec, written_bytes);
}

template <typename stream_type>
task<http1_buffered_response_write_result> write_response_with_local_head(stream_type& stream,
    worker_memory& memory, std::pmr::string* file_chunk_buffer, const http_response& response,
    const http1_buffered_response_plan& response_plan) {
    http_response_head_buffer local_head(memory.allocator<char>());
    co_return co_await write_response_with_scratch(
        stream, memory, local_head, file_chunk_buffer, response, response_plan);
}

template <typename stream_type>
task<http1_buffered_response_write_result> write_response(stream_type& stream, worker_memory& memory,
    http_response_head_buffer* reusable_head, std::pmr::string* file_chunk_buffer,
    const http_response& response, const http1_buffered_response_plan& response_plan) {
    if (reusable_head != nullptr) {
        return write_response_with_scratch(
            stream, memory, *reusable_head, file_chunk_buffer, response, response_plan);
    }
    return write_response_with_local_head(stream, memory, file_chunk_buffer, response, response_plan);
}

}  // namespace ruvia::detail
