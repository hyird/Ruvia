#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "ruvia/core/operation_timeout.h"
#include "ruvia/core/task.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/worker_handle.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/http/http2_connection.h"
#include "ruvia/http/http_pmr_string.h"

#include "client/client_tcp_transport.h"

namespace ruvia::detail {

enum class http2_client_session_start_status : std::uint8_t {
    started,
    // transport_ names the failure, including EOF before the peer SETTINGS.
    transport_failed,
    protocol_failed,
};

struct http2_client_session_start_result final {
    http2_client_session_start_status status_{http2_client_session_start_status::started};
    client_transport_result transport_{};
};

// A protocol failure observed by the reader is reported to the handler as an
// io_failed transport result carrying this error.
[[nodiscard]] inline client_transport_result http2_client_protocol_failure() noexcept {
    return {.status_ = client_transport_status::io_failed, .error_ = std::make_error_code(std::errc::protocol_error)};
}

// The handler owns the stream semantics above the connection: it consumes every
// event, is told when queued DATA of a stream drained, and observes the single
// terminal failure of each started generation (EOF included).
template <typename handler_type>
concept http2_client_session_handler = requires(handler_type& handler, http2_event& event, std::uint32_t stream_id,
    const client_transport_result& transport, std::exception_ptr failure) {
    handler.on_http2_event(event);
    { handler.on_http2_data_drained(stream_id) } noexcept;
    { handler.on_http2_failure(transport, failure) } noexcept;
};

// The one HTTP/2 client connection driver shared by the HTTP client pool and
// the WebSocket client: preface, the sole socket reader and writer, event
// drain and generation-safe failure over a borrowed client_tcp_transport. The
// writer transfers complete frame-bounded batches into its own storage, so a
// feed() or submit during an in-flight write never invalidates the bytes being
// written. Worker-affine and address-stable; the owner joins `tasks` before
// retiring it.
template <typename handler_type>
class http2_client_session final {
public:
    http2_client_session(handler_type& handler, client_tcp_transport& transport, task_scope& tasks,
        const worker_handle& worker_value, std::optional<std::chrono::milliseconds> write_timeout,
        std::pmr::memory_resource* resource)
        : handler_(handler),
          transport_(transport),
          tasks_(tasks),
          write_timeout_(write_timeout),
          resource_(resource),
          write_signal_(worker_value),
          state_signal_(worker_value) {
        // Checked here: an owner composing its session is complete only in its body.
        static_assert(http2_client_session_handler<handler_type>);
    }
    http2_client_session(handler_type&, client_tcp_transport&, task_scope&, worker_handle&&,
        std::optional<std::chrono::milliseconds>, std::pmr::memory_resource*) = delete;

    http2_client_session(const http2_client_session&) = delete;
    http2_client_session& operator=(const http2_client_session&) = delete;

    // Starts a new generation over the freshly connected transport: flushes the
    // client preface, reads until the peer SETTINGS (draining events through
    // the handler), then spawns the reader and writer. The previous
    // generation's tasks must have finished.
    [[nodiscard]] task<http2_client_session_start_result> start(
        http2_connection_options options, const operation_timeout& timeout) {
        if (session_tasks_ != 0) {
            std::terminate();
        }
        options.resource_ = resource_;
        connection_.emplace(::ruvia::http2_connection::client(options));
        generation_ = generation_ + 1 == 0 ? 1 : generation_ + 1;
        failed_ = false;
        draining_ = false;
        std::pmr::string output(resource_);
        std::array<char, read_buffer_bytes> input{};
        for (;;) {
            while (connection_->wants_write()) {
                take_output_batch(output);
                const auto written = co_await transport_.write(output, timeout);
                if (!written.ok()) {
                    connection_.reset();
                    co_return http2_client_session_start_result{
                        .status_ = http2_client_session_start_status::transport_failed, .transport_ = written};
                }
            }
            if (connection_->received_peer_settings()) {
                break;
            }
            const auto read = co_await transport_.read_some(input, timeout);
            if (!read.ok()) {
                connection_.reset();
                co_return http2_client_session_start_result{
                    .status_ = http2_client_session_start_status::transport_failed, .transport_ = read};
            }
            if (!feed(std::string_view(input.data(), read.bytes_))) {
                connection_.reset();
                co_return http2_client_session_start_result{.status_ = http2_client_session_start_status::protocol_failed};
            }
        }
        const auto generation = generation_;
        try {
            ++session_tasks_;
            try {
                tasks_.spawn(run_reader(generation));
            } catch (...) {
                --session_tasks_;
                throw;
            }
            ++session_tasks_;
            try {
                tasks_.spawn(run_writer(generation));
            } catch (...) {
                --session_tasks_;
                throw;
            }
        } catch (...) {
            const auto failure = std::current_exception();
            fail(generation, {}, failure);
            std::rethrow_exception(failure);
        }
        co_return http2_client_session_start_result{};
    }

    // The live connection of the current generation; check usable() first.
    [[nodiscard]] ::ruvia::http2_connection& connection() noexcept {
        return *connection_;
    }
    [[nodiscard]] bool has_connection() const noexcept {
        return connection_.has_value();
    }
    // Accepts new streams: started, not failed and not draining.
    [[nodiscard]] bool usable() const noexcept {
        return connection_.has_value() && session_tasks_ != 0 && !failed_ && !draining_;
    }
    [[nodiscard]] std::size_t available_request_streams(std::size_t local_limit) const noexcept {
        return usable() ? connection_->available_request_streams(local_limit) : 0;
    }

    // Reader or writer of some generation is still running.
    [[nodiscard]] bool running() const noexcept {
        return session_tasks_ != 0;
    }
    [[nodiscard]] bool failed() const noexcept {
        return failed_;
    }
    [[nodiscard]] bool draining() const noexcept {
        return draining_;
    }
    [[nodiscard]] bool writing() const noexcept {
        return writing_;
    }
    [[nodiscard]] std::uint64_t generation() const noexcept {
        return generation_;
    }
    // GOAWAY, an unprocessed request or exhausted stream IDs: no new streams.
    void mark_draining() noexcept {
        draining_ = true;
        state_signal_.notify();
    }
    void wake_writer() noexcept {
        write_signal_.notify();
    }
    // Notified on every state transition, completed write and drained event.
    [[nodiscard]] worker_signal& state_signal() noexcept {
        return state_signal_;
    }

    // Fails the current generation once: aborts the transport, reports the
    // failure to the handler and wakes every waiter.
    void fail(const client_transport_result& transport, std::exception_ptr failure = {}) noexcept {
        fail(generation_, transport, std::move(failure));
    }

    // Requests a RST_STREAM (CANCEL unless the stream's input was malformed);
    // allocation failure fails the connection.
    void submit_reset(std::uint32_t stream_id, http2_error_code error = http2_error_code::cancel) noexcept {
        if (stream_id == 0 || !connection_.has_value() || failed_) {
            return;
        }
        try {
            (void)connection_->submit_reset(stream_id, error);
            write_signal_.notify();
        } catch (...) {
            fail({}, std::current_exception());
        }
    }

    // Drops the retired connection once no generation is running. Escaped
    // DATA credits stay valid; they only reference the memory resource.
    void reset() noexcept {
        if (session_tasks_ == 0) {
            connection_.reset();
        }
    }

private:
    static constexpr std::size_t read_buffer_bytes = 16384;
    static constexpr std::size_t write_batch_bytes = http2_default_max_frame_size;

    void take_output_batch(std::pmr::string& output) {
        output.clear();
        (void)connection_->take_output_batch(write_batch_bytes, output);
    }

    [[nodiscard]] bool feed(std::string_view bytes_value) {
        for (;;) {
            const auto status = connection_->feed(bytes_value);
            drain();
            if (status == http2_feed_result::protocol_failure || connection_->connection_error()) {
                return false;
            }
            if (status != http2_feed_result::events_pending) {
                return true;
            }
        }
    }

    void drain() {
        while (auto event = connection_->next_event()) {
            handler_.on_http2_event(*event);
        }
        for (const auto stream_id : connection_->take_drained_data_streams()) {
            handler_.on_http2_data_drained(stream_id);
        }
        if (connection_->wants_write()) {
            write_signal_.notify();
        }
        state_signal_.notify();
    }

    void fail(std::uint64_t generation, const client_transport_result& transport, std::exception_ptr failure) noexcept {
        if (generation != generation_ || failed_) {
            return;
        }
        failed_ = true;
        draining_ = true;
        transport_.close();
        handler_.on_http2_failure(transport, std::move(failure));
        write_signal_.notify();
        state_signal_.notify();
    }

    void finish_task(std::uint64_t generation) noexcept {
        if (generation != generation_ || session_tasks_ == 0) {
            std::terminate();
        }
        --session_tasks_;
        state_signal_.notify();
    }

    struct task_guard final {
        http2_client_session& session_;
        std::uint64_t generation_;
        ~task_guard() {
            session_.finish_task(generation_);
        }
    };

    [[nodiscard]] task<void> run_reader(std::uint64_t generation) {
        const task_guard guard{*this, generation};
        std::array<char, read_buffer_bytes> input{};
        const operation_timeout unbounded(std::nullopt);
        try {
            while (generation_ == generation && !failed_) {
                const auto read = co_await transport_.read_some(input, unbounded);
                if (!read.ok()) {
                    fail(generation, read, {});
                    co_return;
                }
                if (generation_ != generation || failed_) {
                    co_return;
                }
                if (!feed(std::string_view(input.data(), read.bytes_))) {
                    fail(generation, http2_client_protocol_failure(), {});
                    co_return;
                }
            }
        } catch (...) {
            fail(generation, {}, std::current_exception());
        }
    }

    [[nodiscard]] task<void> run_writer(std::uint64_t generation) {
        const task_guard guard{*this, generation};
        std::pmr::string output(resource_);
        try {
            for (;;) {
                while (generation_ == generation && !failed_ && connection_->wants_write()) {
                    take_output_batch(output);
                    writing_ = true;
                    const auto written = co_await transport_.write(output, operation_timeout(write_timeout_));
                    writing_ = false;
                    // Retain ordinary heads and frame-sized DATA batches, but
                    // return an unexpectedly larger indivisible frame before
                    // the connection idles.
                    http_clear_pmr_string_retaining_small(output, 2 * write_batch_bytes);
                    if (!written.ok()) {
                        fail(generation, written, {});
                        co_return;
                    }
                    state_signal_.notify();
                }
                if (generation_ != generation || failed_) {
                    co_return;
                }
                co_await write_signal_.wait();
            }
        } catch (...) {
            writing_ = false;
            fail(generation, {}, std::current_exception());
        }
    }

    handler_type& handler_;
    client_tcp_transport& transport_;
    task_scope& tasks_;
    std::optional<std::chrono::milliseconds> write_timeout_;
    std::pmr::memory_resource* resource_;
    std::optional<::ruvia::http2_connection> connection_;
    worker_signal write_signal_;
    worker_signal state_signal_;
    std::uint64_t generation_{0};
    std::size_t session_tasks_{0};
    bool failed_{false};
    bool draining_{false};
    bool writing_{false};
};

}  // namespace ruvia::detail
