#pragma once

// Per-stream async plumbing for the sans-I/O HTTP/2 session.
//
// Inbound bytes consumed asynchronously (a websocket tunnel or streaming request
// body) live in a Web-owned queue. The HTTP core retains receive-window debt for
// each delivered DATA event; these consumers acknowledge it only after the queue
// drains, so a suspended handler naturally backpressures the peer.
//
// The same-executor discipline of the session (reader, writer and handlers all run
// on the connection's executor) means these never race: a signal wake
// while nothing is waiting is a no-op, and every consumer re-checks its condition
// before suspending, so no wakeup is lost.

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <system_error>

#include "ruvia/core/bytes.h"
#include "ruvia/core/pmr_string.h"
#include "ruvia/core/task.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/http/http2_connection.h"

#include "http/http_stream_read_result.h"
#include "http2/http2_data_output_budget.h"
#include "http2/http2_sans_io_send_window.h"
#include "http2/http2_sans_io_stream_runtime.h"

namespace ruvia::detail {

// Duplex CONNECT transport driven by one HTTP/2 session reader and writer.
// Queued input retains its receive-window credit until the application consumes it.
template <typename executor_type>
class http2_sans_io_tunnel_transport final {
public:
    http2_sans_io_tunnel_transport(ruvia::http2_connection& connection, std::uint32_t stream_id,
        http2_sans_io_body_queue& body_queue, http2_sans_io_stream_signal& signal, worker_signal& write_signal,
        http2_data_output_budget& output_budget, executor_type executor) noexcept
        : connection_(connection),
          stream_id_(stream_id),
          body_queue_(body_queue),
          signal_(signal),
          write_signal_(write_signal),
          output_budget_(&output_budget),
          executor_(executor) {}

    http2_sans_io_tunnel_transport(ruvia::http2_connection& connection, std::uint32_t stream_id,
        http2_sans_io_body_queue& body_queue, http2_sans_io_stream_signal& signal, worker_signal& write_signal,
        executor_type executor) noexcept
        : connection_(connection),
          stream_id_(stream_id),
          body_queue_(body_queue),
          signal_(signal),
          write_signal_(write_signal),
          executor_(executor) {}

    [[nodiscard]] executor_type executor() const noexcept {
        return executor_;
    }

    [[nodiscard]] task<http_stream_read_result> read_more(std::pmr::string& buffer) {
        for (;;) {
            if (aborted_ || signal_.terminated()) {
                co_return http_stream_read_result::make_failure(aborted_
                                                                    ? std::make_error_code(std::errc::operation_canceled)
                                                                    : signal_.terminal_error());
            }
            const auto receive_status = connection_.stream_receive_status(stream_id_);
            if (receive_status == http2_stream_receive_status::closed) {
                co_return http_stream_read_result::make_failure(
                    std::make_error_code(std::errc::connection_reset));
            }
            const auto chunk = body_queue_.pop();
            // pop() returns the previous chunk's receive-window credit; flush
            // the WINDOW_UPDATE even if this reader suspends below.
            if (connection_.wants_write()) {
                wake_writer();
            }
            if (!chunk.empty()) {
                buffer.append(chunk.data(), chunk.size());
                co_return http_stream_read_result::make_data();
            }
            if (!body_queue_.empty()) {
                continue;
            }
            if (receive_status == http2_stream_receive_status::ended) {
                co_return http_stream_read_result::make_end();
            }
            if (aborted_ || signal_.terminated()) {
                co_return http_stream_read_result::make_failure(aborted_
                                                                    ? std::make_error_code(std::errc::operation_canceled)
                                                                    : signal_.terminal_error());
            }
            co_await signal_.wait();
        }
    }

    [[nodiscard]] task<std::error_code> write_bytes(
        std::string_view bytes_value, http_stream_end disposition) {
        const auto terminal = disposition == http_stream_end::end
                                  ? http2_end_stream::end_stream
                                  : http2_end_stream::keep_open;
        constexpr std::size_t submit_chunk_bytes = http2_data_output_credit_bytes;
        std::size_t offset = 0;
        bool submitted_empty_terminal = false;
        do {
            const auto count = bytes_value.empty() ? 0 : std::min(submit_chunk_bytes, bytes_value.size() - offset);
            const auto chunk = bytes_value.substr(offset, count);
            const bool last = offset + count == bytes_value.size();
            const auto end = last ? terminal : http2_end_stream::keep_open;
            for (;;) {
                if (aborted_ || signal_.terminated()) {
                    co_return aborted_ ? std::make_error_code(std::errc::operation_canceled)
                                       : signal_.terminal_error();
                }
                if (chunk.empty() && end == http2_end_stream::end_stream &&
                    connection_.has_queued_data(stream_id_)) {
                    const auto wait_result = co_await wait_for_send_window();
                    if (wait_result.aborted() != nullptr) {
                        co_return signal_.terminated() ? signal_.terminal_error()
                                                       : std::make_error_code(std::errc::connection_reset);
                    }
                    continue;
                }
                if (output_budget_ != nullptr && !chunk.empty()) {
                    for (;;) {
                        const auto window = connection_.send_window_state(stream_id_);
                        if (!window) {
                            co_return std::make_error_code(std::errc::connection_reset);
                        }
                        if (aborted_ || signal_.terminated()) {
                            co_return aborted_ ? std::make_error_code(std::errc::operation_canceled)
                                               : signal_.terminal_error();
                        }
                        if (window->available_ != 0) {
                            break;
                        }
                        co_await output_budget_->wait_for_change();
                        if (aborted_ || signal_.terminated()) {
                            co_return aborted_ ? std::make_error_code(std::errc::operation_canceled)
                                               : signal_.terminal_error();
                        }
                    }
                    if (!(co_await output_budget_->acquire(stream_id_, signal_)) || aborted_ ||
                        signal_.terminated()) {
                        co_return aborted_ ? std::make_error_code(std::errc::operation_canceled)
                                           : signal_.terminal_error();
                    }
                }
                const auto result_value = connection_.submit_data(stream_id_, chunk, end);
                if (output_budget_ != nullptr &&
                    (result_value == http2_data_submit_status::accepted ||
                        result_value == http2_data_submit_status::queued)) {
                    output_budget_->note_data_submitted(stream_id_, chunk.size());
                }
                wake_writer();
                if (result_value != http2_data_submit_status::accepted &&
                    result_value != http2_data_submit_status::queued) {
                    if (output_budget_ != nullptr && !chunk.empty()) {
                        output_budget_->release(stream_id_);
                    }
                }
                if (result_value == http2_data_submit_status::accepted) {
                    break;
                }
                if (result_value == http2_data_submit_status::closed) {
                    co_return std::make_error_code(std::errc::connection_reset);
                }
                if (result_value == http2_data_submit_status::invalid_state ||
                    result_value == http2_data_submit_status::content_length_exceeded ||
                    result_value == http2_data_submit_status::content_length_incomplete) {
                    co_return std::make_error_code(std::errc::protocol_error);
                }
                const auto wait_result = co_await wait_for_send_window();
                if (wait_result.aborted() != nullptr) {
                    co_return signal_.terminated() ? signal_.terminal_error()
                                                   : std::make_error_code(std::errc::connection_reset);
                }
                if (result_value == http2_data_submit_status::queued) {
                    break;  // core owns this bounded chunk; wait before next submit
                }
            }
            offset += count;
            submitted_empty_terminal = bytes_value.empty();
            if (!bytes_value.empty() && offset < bytes_value.size()) {
                const auto wait_result = co_await wait_for_send_window();
                if (wait_result.aborted() != nullptr) {
                    co_return signal_.terminated() ? signal_.terminal_error()
                                                   : std::make_error_code(std::errc::connection_reset);
                }
            }
        } while (offset < bytes_value.size() || (!submitted_empty_terminal && bytes_value.empty()));
        co_return std::error_code{};
    }

    void abort() noexcept {
        if (aborted_) {
            return;
        }
        aborted_ = true;
        if (output_budget_ != nullptr) {
            output_budget_->release(stream_id_);
            output_budget_->wake();
        }
        try {
            (void)connection_.submit_reset(stream_id_, http2_error_code::cancel);
        } catch (...) {
            // abort() is best-effort teardown; callers cannot observe reset
            // serialization failure on this noexcept cleanup path.
        }
        signal_.wake();
        wake_writer();
    }

private:
    [[nodiscard]] task<http2_send_window_wait_result> wait_for_send_window() {
        for (;;) {
            if (aborted_ || signal_.terminated() ||
                connection_.stream_receive_status(stream_id_) == http2_stream_receive_status::closed) {
                co_return http2_send_window_wait_result::make_aborted();
            }
            if (!connection_.has_queued_data(stream_id_)) {
                co_return http2_send_window_wait_result::make_ready();
            }
            co_await signal_.wait();
        }
    }

    void wake_writer() noexcept {
        write_signal_.notify();
    }

    ruvia::http2_connection& connection_;
    std::uint32_t stream_id_;
    http2_sans_io_body_queue& body_queue_;
    http2_sans_io_stream_signal& signal_;
    worker_signal& write_signal_;
    http2_data_output_budget* output_budget_{nullptr};
    executor_type executor_;
    bool aborted_{false};
};

}  // namespace ruvia::detail
