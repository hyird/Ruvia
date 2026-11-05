#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory_resource>
#include <span>
#include <string_view>
#include <system_error>
#include <utility>

#include <asio.hpp>

#include "ruvia/core/async.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/http/http_io_buffer.h"
#include "ruvia/http/http_limits.h"

#include "server/http_connection_state.h"
#include "server/inbound_transport_error.h"

namespace ruvia::detail {

// HTTP/1 流式响应的读侧终止检测：请求体完整消费后由 route 自动启动（同 Go
// net/http background read）。单独到达的请求遇对端 FIN/RST 请求当前请求的 stop。
// pipeline 内（本请求排在前一请求之后到达，或其后已有后续请求字节）的干净结束
// （TCP FIN、TLS close_notify）是请求方向半关闭：不取消当前响应，并经
// request_input_ended() 交给连接，已收到的请求照常服务、不再读取；RST、TLS 截断
// 与其他错误始终中止。route 负责 cancel/join 后再释放 transport。
template <typename stream_type>
class http1_peer_eof_monitor final {
public:
    // `request_stop` is the request's own source; it outlives the monitor.
    // `request_pipelined`: the request itself arrived behind another one.
    http1_peer_eof_monitor(stream_type& stream, const worker_handle& worker, std::pmr::memory_resource* resource,
        stop_source& request_stop, bool request_pipelined, std::size_t existing_pipeline_bytes)
        : stream_(stream),
          request_stop_(request_stop),
          tasks_(worker, {.resource_ = resource}),
          input_(std::pmr::polymorphic_allocator<char>(resource)),
          allowance_(max_http_header_bytes - std::min(max_http_header_bytes, existing_pipeline_bytes)),
          pipelined_(request_pipelined || existing_pipeline_bytes != 0) {}

    void start() {
        tasks_.spawn(read_peer());
    }

    [[nodiscard]] bool peer_aborted() const noexcept {
        return peer_aborted_;
    }

    // The peer ended its request direction behind pipelined bytes. The
    // transport holds no more input; reading it again would only report the
    // same end. Final once join() has returned.
    [[nodiscard]] bool request_input_ended() const noexcept {
        return request_input_ended_;
    }

    [[nodiscard]] std::string_view pipeline() const noexcept {
        return input_.view();
    }

    void ensure_writable() const {
        if (failure_ != nullptr) {
            std::rethrow_exception(failure_);
        }
        if (peer_aborted_) {
            throw inbound_transport_error(asio::error::operation_aborted,
                "HTTP response stream peer has disconnected");
        }
    }

    // The sink requests the request stop itself on every write failure.
    void write_failed(const std::error_code& error) noexcept {
        peer_aborted_ = peer_aborted_ || is_inbound_connection_end(error);
        cancel();
    }

    void cancel() noexcept {
        // Set before emitting: an already-queued successful completion must
        // retain its bytes but must not schedule another read after teardown.
        closing_ = true;
        cancellation_.emit(asio::cancellation_type::terminal);
    }

    task<void> join() {
        if (!joined_) {
            joined_ = true;
            co_await tasks_.join();
        }
        if (failure_ != nullptr) {
            std::rethrow_exception(failure_);
        }
    }

private:
    task<void> read_peer() {
        try {
            // Stop once this bounded look-ahead is full. No input is discarded;
            // a reset behind that many pipelined bytes then surfaces only once
            // the handler finishes and the session consumes those requests.
            while (!closing_ && input_.size() < allowance_) {
                // A quiet SSE needs no look-ahead allocation. After the first
                // application byte, grow like the connection read buffer
                // instead of reserving the full 64 KiB header allowance.
                char first_byte;
                const bool first_read = input_.capacity() == 0;
                if (!first_read && input_.size() == input_.capacity()) {
                    input_.reserve(std::min(
                        allowance_, std::max(initial_read_buffer_bytes, input_.capacity() * 2)));
                }
                const auto space = first_read ? std::span<char>(&first_byte, 1) : input_.prepare_spare();
                // Cancel this operation, not the socket's concurrent writer.
                // Asio's SSL io_op feeds received encrypted input into its
                // engine before honoring read cancellation; it remains there
                // for the session's next read after this task has joined.
                const auto completion = co_await async_asio<std::size_t>([this, space](auto handler) mutable {
                    stream_.async_read_some(asio::buffer(space),
                        asio::bind_cancellation_slot(cancellation_.slot(), std::move(handler)));
                });
                // A terminal cancellation can race a successful read. Preserve
                // every returned byte before inspecting the shutdown state.
                if (first_read) {
                    if (completion.result() != 0) {
                        input_.reserve(std::min(initial_read_buffer_bytes, allowance_));
                        input_.append(std::string_view(&first_byte, completion.result()));
                    }
                } else {
                    input_.commit(completion.result());
                }
                const auto error = completion.error_code();
                if (error) {
                    if (closing_ && error == asio::error::operation_aborted) {
                        co_return;
                    }
                    if (is_inbound_connection_end(error)) {
                        // RFC 9112 section 9.6: within a pipeline -- this request
                        // arrived behind another, or more follow it -- a clean
                        // end (TCP FIN, TLS close_notify) closes only the request
                        // direction; every request received gets its response.
                        // That no longer depends on when the FIN arrives. A
                        // request that arrived alone, a reset, and a TLS stream
                        // truncated without close_notify end the response.
                        if (error == asio::error::eof && (pipelined_ || input_.size() != 0)) {
                            request_input_ended_ = true;
                            co_return;
                        }
                        peer_aborted_ = true;
                        request_stop_.request_stop();
                        co_return;
                    }
                    throw inbound_transport_error(error, "failed to monitor HTTP response stream peer");
                }
            }
        } catch (...) {
            failure_ = std::current_exception();
            request_stop_.request_stop();
        }
    }

    stream_type& stream_;
    stop_source& request_stop_;
    task_scope tasks_;
    http_io_buffer input_;
    asio::cancellation_signal cancellation_;
    std::size_t allowance_;
    std::exception_ptr failure_;
    bool pipelined_;
    bool closing_{};
    bool peer_aborted_{};
    bool request_input_ended_{};
    bool joined_{};
};

}  // namespace ruvia::detail
