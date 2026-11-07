#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "ruvia/core/operation_timeout.h"
#include "ruvia/core/task.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/worker_handle.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/core/worker_timer.h"
#include "ruvia/http/http2_connection.h"
#include "ruvia/http/http_io_buffer.h"
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

// Product limit: the longest a graceful close waits for its GOAWAY to reach the
// transport before it closes anyway (an unread peer, or write_timeout nullopt).
inline constexpr std::chrono::milliseconds http2_client_goaway_flush_time{1000};

enum class http2_client_data_status : std::uint8_t {
    // Every byte was submitted and its output reached the transport, or the
    // session ended meanwhile; the caller judges that end itself.
    submitted,
    // The caller's stop condition held, or the session ended, before every
    // byte was submitted.
    stopped,
    // The core refused the DATA: the stream no longer sends, or the input
    // contradicts its declared length or state (std::logic_error).
    rejected,
};

// The handler owns stream semantics above the connection: it consumes every
// application event and observes each generation's terminal failure. Payload
// producers wait on state_signal(), also notified for SETTINGS/WINDOW_UPDATE;
// request submitters wait on capacity_signal().
template <typename handler_type>
concept http2_client_session_handler = requires(handler_type& handler, http2_event& event,
    const client_transport_result& transport, std::exception_ptr failure) {
    handler.on_http2_event(event);
    { handler.on_http2_failure(transport, failure) } noexcept;
};

// The one HTTP/2 client connection driver shared by the HTTP client pool and
// the WebSocket client: preface, the sole socket reader and writer, event
// drain, DATA submission under flow control, PING liveness and
// generation-safe failure over a borrowed client_tcp_transport. The writer
// transfers complete frame-bounded batches into its own storage, so a feed()
// or submit during an in-flight write never invalidates the bytes being
// written. Worker-affine and address-stable; the owner joins `tasks` before
// retiring it.
template <typename handler_type>
class http2_client_session final {
public:
    // `ping_interval` bounds silence: no received frame for one interval sends
    // a PING, and another silent interval fails the session with timeout.
    http2_client_session(handler_type& handler, client_tcp_transport& transport, task_scope& tasks,
        const worker_handle& worker_value, std::optional<std::chrono::milliseconds> write_timeout,
        std::optional<std::chrono::milliseconds> ping_interval, std::pmr::memory_resource* resource)
        : handler_(handler),
          transport_(transport),
          tasks_(tasks),
          worker_(worker_value),
          write_timeout_(write_timeout),
          ping_interval_(ping_interval),
          resource_(resource),
          read_buffer_(resource),
          write_buffer_(resource),
          write_signal_(worker_value),
          state_signal_(worker_value),
          capacity_signal_(worker_value) {
        // Checked here: an owner composing its session is complete only in its body.
        static_assert(http2_client_session_handler<handler_type>);
    }
    http2_client_session(handler_type&, client_tcp_transport&, task_scope&, worker_handle&&,
        std::optional<std::chrono::milliseconds>, std::optional<std::chrono::milliseconds>,
        std::pmr::memory_resource*) = delete;

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
        closing_ = false;
        request_capacity_ = 0;
        read_buffer_.clear();
        // Only the prefix reported by read_some reaches the parser.
        const auto input = read_buffer_.prepare(read_buffer_bytes);
        for (;;) {
            while (connection_->wants_write()) {
                take_output_batch(write_buffer_);
                const auto written = co_await transport_.write(write_buffer_, timeout);
                http_clear_pmr_string_retaining_small(write_buffer_, write_batch_bytes);
                if (!written.ok()) {
                    reset();
                    co_return http2_client_session_start_result{
                        .status_ = http2_client_session_start_status::transport_failed, .transport_ = written};
                }
            }
            if (connection_->received_peer_settings()) {
                break;
            }
            const auto read = co_await transport_.read_some(input, timeout);
            if (!read.ok()) {
                reset();
                co_return http2_client_session_start_result{
                    .status_ = http2_client_session_start_status::transport_failed, .transport_ = read};
            }
            if (!feed(std::string_view(input.data(), read.bytes_))) {
                reset();
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
            if (ping_interval_) {
                last_input_ = std::chrono::steady_clock::now();
                ping_pending_ = false;
                schedule_ping(generation,
                    worker_timer_saturating_deadline(last_input_, worker_timer_saturating_duration_cast(*ping_interval_)));
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
    // begin_close() queued its GOAWAY and the generation has not ended yet.
    [[nodiscard]] bool closing() const noexcept {
        return closing_ && !failed_;
    }
    [[nodiscard]] std::uint64_t generation() const noexcept {
        return generation_;
    }
    // GOAWAY, an unprocessed request or exhausted stream IDs: no new streams.
    void mark_draining() noexcept {
        draining_ = true;
        state_signal_.notify();
        capacity_signal_.notify();
    }
    void wake_writer() noexcept {
        if (connection_.has_value() && !failed_ && connection_->wants_write()) {
            write_signal_.notify();
        }
    }
    // Notified after every input batch, state transition and completed write.
    [[nodiscard]] worker_signal& state_signal() noexcept {
        return state_signal_;
    }
    // Notified only when the request-stream capacity grew (a stream closed or
    // the peer raised SETTINGS_MAX_CONCURRENT_STREAMS) or the session stopped
    // admitting streams, so a parked request submitter is not woken by
    // unrelated input. A submitter's own cancellation notifies it explicitly.
    [[nodiscard]] worker_signal& capacity_signal() noexcept {
        return capacity_signal_;
    }

    // Fails the current generation once: aborts the transport, reports the
    // failure to the handler and wakes every waiter.
    void fail(const client_transport_result& transport, std::exception_ptr failure = {}) noexcept {
        fail(generation_, transport, std::move(failure));
    }

    // The one client-initiated close of a running generation (RFC 9113
    // section 6.8: GOAWAY before closing). Stops admitting streams, queues
    // GOAWAY(NO_ERROR) and lets the writer fail the generation with `transport`
    // once every queued frame reached the transport, or after
    // http2_client_goaway_flush_time. Streams still open are not cancelled
    // here; the bound ends them with the generation. Returns true while that
    // close is pending. Without a running generation that can send the frame
    // (or a timer to bound it) the generation fails at once and this returns
    // false.
    bool begin_close(const client_transport_result& transport) noexcept {
        if (closing()) {
            return true;
        }
        if (!connection_.has_value() || session_tasks_ == 0 || failed_) {
            fail(transport);
            return false;
        }
        const auto generation = generation_;
        close_result_ = transport;
        try {
            // A client drain queues its final GOAWAY at once; idempotent.
            connection_->begin_drain();
            if (worker_.schedule_timer(close_timer_, worker_timer_deadline_after(http2_client_goaway_flush_time),
                    [this, generation](worker_timer_outcome outcome) noexcept {
                        if (outcome == worker_timer_outcome::expired) {
                            fail(generation, close_result_, {});
                        }
                    }) != worker_timer_schedule_status::scheduled) {
                // A stopping worker cannot bound the flush.
                fail(generation, transport, {});
                return false;
            }
        } catch (...) {
            fail(generation, transport, {});
            return false;
        }
        closing_ = true;
        mark_draining();
        // Also wakes an idle writer when the GOAWAY was already sent.
        write_signal_.notify();
        return true;
    }

    // Requests a RST_STREAM (CANCEL unless the stream's input was malformed);
    // allocation failure fails the connection.
    void submit_reset(std::uint32_t stream_id, http2_error_code error = http2_error_code::cancel) noexcept {
        if (stream_id == 0 || !connection_.has_value() || failed_) {
            return;
        }
        try {
            if (connection_->submit_reset(stream_id, error) == http2_submit_status::accepted &&
                connection_->wants_write()) {
                write_signal_.notify();
            }
        } catch (...) {
            fail({}, std::current_exception());
        }
    }

    // Submits `bytes` on `stream_id` in DATA frames of at most `chunk_bytes`,
    // waiting for each batch to reach the transport and for send window.
    // An empty input ending the stream is still one DATA frame, sent only
    // while both send windows are positive (RFC 9113 section 6.9).
    // `stop()` is checked before each submission and after each wait.
    // Exceptions other than the core's std::logic_error rejection propagate.
    template <typename stop_type>
    [[nodiscard]] task<http2_client_data_status> send_data(std::uint32_t stream_id, std::string_view bytes,
        http2_end_stream end_stream, std::size_t chunk_bytes, stop_type stop, bool* end_submitted = nullptr) {
        const auto generation = generation_;
        bool end_pending = http2_ends_stream(end_stream);
        while (!bytes.empty() || end_pending) {
            if (ended(generation) || stop()) {
                co_return http2_client_data_status::stopped;
            }
            if (connection_->stream_aborted(stream_id)) {
                co_return http2_client_data_status::rejected;
            }
            http2_data_submit_result submitted{};
            try {
                submitted = connection_->submit_data(stream_id, bytes, end_stream, chunk_bytes);
            } catch (const std::logic_error&) {
                co_return http2_client_data_status::rejected;
            }
            bytes.remove_prefix(submitted.consumed_bytes_);
            if (submitted.status_ == http2_data_submit_status::complete) {
                if (end_pending && end_submitted != nullptr) {
                    *end_submitted = true;
                }
                end_pending = false;
            }
            wake_writer();
            if (submitted.status_ == http2_data_submit_status::closed) {
                co_return http2_client_data_status::rejected;
            }
            if (submitted.status_ == http2_data_submit_status::expectation_pending) {
                // 100 (Continue) and the continue timeout both wake state waiters.
                co_await state_signal_.wait();
                continue;
            }
            co_await wait_output_flushed(stop);
            if (submitted.status_ == http2_data_submit_status::backpressured) {
                co_await wait_send_window(generation, stream_id, stop);
            }
        }
        co_return http2_client_data_status::submitted;
    }

    // Ends a request stream with `trailers`, or without them with an empty
    // END_STREAM DATA frame, which waits for positive send windows like
    // send_data(). Rejected when the stream no longer sends or the content is
    // incomplete (std::logic_error).
    template <typename stop_type>
    [[nodiscard]] task<http2_client_data_status> finish_request(
        std::uint32_t stream_id, std::span<const http_header_view> trailers, stop_type stop) {
        const auto generation = generation_;
        for (;;) {
            if (ended(generation) || stop()) {
                co_return http2_client_data_status::stopped;
            }
            if (connection_->stream_aborted(stream_id)) {
                co_return http2_client_data_status::rejected;
            }
            http2_finish_status finished{};
            try {
                finished = connection_->finish_request(stream_id, trailers);
            } catch (const std::logic_error&) {
                co_return http2_client_data_status::rejected;
            }
            wake_writer();
            if (finished == http2_finish_status::accepted) {
                co_await wait_output_flushed(stop);
                co_return http2_client_data_status::submitted;
            }
            if (finished != http2_finish_status::backpressured) {
                co_return http2_client_data_status::rejected;
            }
            co_await wait_send_window(generation, stream_id, stop);
        }
    }

    [[nodiscard]] bool output_flushed() const noexcept {
        return !writing_ && connection_.has_value() && !connection_->wants_write();
    }

    // Waits until every queued frame reached the transport, the session ended
    // or `stop()` holds.
    template <typename stop_type>
    [[nodiscard]] task<void> wait_output_flushed(stop_type stop) {
        const auto generation = generation_;
        while (!ended(generation) && (connection_->wants_write() || writing_) && !stop()) {
            co_await state_signal_.wait();
        }
    }

    // Waits until the stream has send window again, WINDOW_UPDATE and
    // SETTINGS producing no stream event, or the session ended, the stream
    // was aborted or `stop()` holds.
    template <typename stop_type>
    [[nodiscard]] task<void> wait_send_window(std::uint64_t generation, std::uint32_t stream_id, stop_type& stop) {
        while (!ended(generation) && !stop()) {
            const auto window = connection_->send_window_state(stream_id);
            if (!window || window->available_ != 0 || connection_->stream_aborted(stream_id)) {
                co_return;
            }
            co_await state_signal_.wait();
        }
    }

    // Drops the retired connection once no generation is running. Escaped
    // DATA credits stay valid; they only reference the memory resource.
    void reset() noexcept {
        if (session_tasks_ == 0) {
            connection_.reset();
            read_buffer_.clear();
            std::pmr::string(resource_).swap(write_buffer_);
        }
    }

private:
    static constexpr std::size_t read_buffer_bytes = 16384;
    static constexpr std::size_t write_batch_bytes = 2 * http2_default_max_frame_size;

    [[nodiscard]] bool ended(std::uint64_t generation) const noexcept {
        return generation != generation_ || failed_ || !connection_.has_value();
    }

    void schedule_ping(std::uint64_t generation, std::chrono::steady_clock::time_point deadline) {
        (void)worker_.schedule_timer(ping_timer_, deadline, [this, generation](worker_timer_outcome outcome) noexcept {
            if (outcome == worker_timer_outcome::expired) {
                check_liveness(generation);
            }
        });
    }

    // Any received frame, the PING ACK included, proves liveness (as Go's
    // ReadIdleTimeout): one silent interval sends a PING, and the session
    // fails only when no input arrived during the interval after it. Input is
    // compared with when the PING was sent, not with the timer's firing time,
    // which may trail its deadline.
    void check_liveness(std::uint64_t generation) noexcept {
        if (ended(generation) || session_tasks_ == 0) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        const auto interval = worker_timer_saturating_duration_cast(*ping_interval_);
        try {
            if (ping_pending_ && last_input_ > ping_sent_at_) {
                ping_pending_ = false;
            }
            if (ping_pending_) {
                if (now - ping_sent_at_ >= interval) {
                    fail(generation,
                        {.status_ = client_transport_status::aborted, .abort_ = client_abort_reason::timeout}, {});
                } else {
                    schedule_ping(generation, worker_timer_saturating_deadline(ping_sent_at_, interval));
                }
                return;
            }
            if (const auto idle_deadline = worker_timer_saturating_deadline(last_input_, interval); idle_deadline > now) {
                schedule_ping(generation, idle_deadline);
                return;
            }
            if (connection_->submit_ping() != http2_submit_status::accepted) {
                // The connection already failed; the reader reports it.
                return;
            }
            ping_pending_ = true;
            ping_sent_at_ = now;
            wake_writer();
            schedule_ping(generation, worker_timer_saturating_deadline(now, interval));
        } catch (...) {
            fail(generation, {}, std::current_exception());
        }
    }

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
        if (connection_->wants_write()) {
            write_signal_.notify();
        }
        publish_state();
    }

    // Every input batch and completed write may close streams or apply peer
    // SETTINGS; only a grown request capacity wakes capacity waiters.
    void publish_state() noexcept {
        state_signal_.notify();
        const auto capacity =
            usable() ? connection_->available_request_streams(std::numeric_limits<std::size_t>::max()) : 0;
        if (capacity > request_capacity_) {
            capacity_signal_.notify();
        }
        request_capacity_ = capacity;
    }

    void fail(std::uint64_t generation, const client_transport_result& transport, std::exception_ptr failure) noexcept {
        if (generation != generation_ || failed_) {
            return;
        }
        failed_ = true;
        draining_ = true;
        ping_timer_.cancel();
        close_timer_.cancel();
        transport_.close();
        handler_.on_http2_failure(transport, std::move(failure));
        write_signal_.notify();
        state_signal_.notify();
        capacity_signal_.notify();
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
        const auto input = read_buffer_.prepare(read_buffer_bytes);
        const operation_timeout unbounded(std::nullopt);
        try {
            while (generation_ == generation && !failed_) {
                const auto read = co_await transport_.read_some(input, unbounded);
                if (!read.ok()) {
                    // A peer can close immediately after receiving our final
                    // END_STREAM, before the socket write callback is dispatched.
                    // Let that callback establish success or failure first.
                    if (read.status_ == client_transport_status::eof) {
                        while (generation_ == generation && writing_ && !failed_) {
                            co_await state_signal_.wait();
                        }
                        if (generation_ != generation || failed_) {
                            co_return;
                        }
                    }
                    fail(generation, read, {});
                    co_return;
                }
                if (generation_ != generation || failed_) {
                    co_return;
                }
                if (ping_interval_) {
                    last_input_ = std::chrono::steady_clock::now();
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
        try {
            for (;;) {
                while (generation_ == generation && !failed_ && connection_->wants_write()) {
                    take_output_batch(write_buffer_);
                    writing_ = true;
                    const auto written =
                        co_await transport_.write(write_buffer_, operation_timeout(write_timeout_));
                    writing_ = false;
                    // Retain ordinary heads and frame-sized DATA batches, but
                    // return an unexpectedly larger indivisible frame before
                    // the connection idles.
                    http_clear_pmr_string_retaining_small(write_buffer_, write_batch_bytes);
                    if (!written.ok()) {
                        fail(generation, written, {});
                        co_return;
                    }
                    publish_state();
                }
                if (generation_ != generation || failed_) {
                    co_return;
                }
                if (closing_) {
                    // Every frame queued up to and after the GOAWAY was written.
                    fail(generation, close_result_, {});
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
    const worker_handle& worker_;
    std::optional<std::chrono::milliseconds> write_timeout_;
    std::optional<std::chrono::milliseconds> ping_interval_;
    std::pmr::memory_resource* resource_;
    std::optional<::ruvia::http2_connection> connection_;
    http_io_buffer read_buffer_;
    std::pmr::string write_buffer_;
    worker_signal write_signal_;
    worker_signal state_signal_;
    worker_signal capacity_signal_;
    std::uint64_t generation_{0};
    std::size_t session_tasks_{0};
    // Request-stream capacity at the last publish_state().
    std::size_t request_capacity_{0};
    // When the reader last received bytes, while ping_interval_ is set.
    std::chrono::steady_clock::time_point last_input_{};
    // When the outstanding liveness PING was queued.
    std::chrono::steady_clock::time_point ping_sent_at_{};
    // The generation's end once begin_close() flushed its GOAWAY.
    client_transport_result close_result_{};
    bool failed_{false};
    bool draining_{false};
    bool writing_{false};
    // begin_close() is pending for the current generation.
    bool closing_{false};
    // A liveness PING is outstanding: no input arrived since ping_sent_at_.
    bool ping_pending_{false};
    // Last: destroyed first, before any state their completions read.
    worker_timer_registration close_timer_;
    worker_timer_registration ping_timer_;
};

}  // namespace ruvia::detail
