#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "ruvia/core/operation_deadline.h"
#include "ruvia/core/pool_lease_scheduler.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/task.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/worker_handle.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/core/worker_timer.h"
#ifdef RUVIA_ENABLE_HTTP2
#include "ruvia/http/http2_connection.h"
#endif
#include "ruvia/http/http_io_buffer.h"
#include "ruvia/http/http_priority.h"
#include "ruvia/web/http_client_types.h"

#include "client/client_tcp_transport.h"
#include "client/http_client_response_state.h"

namespace ruvia::detail {

class http_client_output_queue;
class http_client_request_storage;
class http_client_pool;
class http_client_response_state;
struct http_client_websocket_handshake;

#ifdef RUVIA_ENABLE_HTTP2
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

class http_client_connection;

// The HTTP/2 client connection driver: preface, the sole socket reader and
// writer, event drain, DATA submission under flow control, PING liveness and
// generation-safe failure over a borrowed client_tcp_transport. The writer
// transfers complete frame-bounded batches into its own storage, so a feed()
// or submit during an in-flight write never invalidates the bytes being
// written. Worker-affine and address-stable; the owner joins `tasks` before
// retiring it.
class http2_client_session final {
public:
    // `ping_interval` bounds silence: no received frame for one interval sends
    // a PING, and another silent interval fails the session with timeout.
    http2_client_session(http_client_connection& handler, client_tcp_transport& transport, task_scope& tasks,
        const worker_handle& worker_value, std::optional<timeout_duration> ping_interval);
    http2_client_session(http_client_connection&, client_tcp_transport&, task_scope&, worker_handle&&,
        std::optional<timeout_duration>) = delete;

    http2_client_session(const http2_client_session&) = delete;
    http2_client_session& operator=(const http2_client_session&) = delete;

    // Starts a new generation over the freshly connected transport: flushes the
    // client preface, reads until the peer SETTINGS (draining events through
    // the handler), then spawns the reader and writer. The previous
    // generation's tasks must have finished.
    [[nodiscard]] task<http2_client_session_start_result> start(
        http2_connection_options options, const operation_deadline& timeout);

    // The live connection of the current generation; check usable() first.
    [[nodiscard]] ::ruvia::http2_connection& connection() noexcept;
    [[nodiscard]] bool has_connection() const noexcept;
    // Accepts new streams: started, not failed and not draining.
    [[nodiscard]] bool usable() const noexcept;
    [[nodiscard]] std::size_t available_request_streams(std::size_t local_limit) const noexcept;

    // Reader or writer of some generation is still running.
    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] bool failed() const noexcept;
    [[nodiscard]] bool draining() const noexcept;
    [[nodiscard]] bool writing() const noexcept;
    // begin_close() queued its GOAWAY and the generation has not ended yet.
    [[nodiscard]] bool closing() const noexcept;
    [[nodiscard]] std::uint64_t generation() const noexcept;
    // GOAWAY, an unprocessed request or exhausted stream IDs: no new streams.
    void mark_draining() noexcept;
    void wake_writer() noexcept;
    // Notified after every input batch, state transition and completed write.
    [[nodiscard]] worker_signal& state_signal() noexcept;
    // Notified only when the request-stream capacity grew (a stream closed or
    // the peer raised SETTINGS_MAX_CONCURRENT_STREAMS) or the session stopped
    // admitting streams, so a parked request submitter is not woken by
    // unrelated input. A submitter's own cancellation notifies it explicitly.
    [[nodiscard]] worker_signal& capacity_signal() noexcept;

    // Fails the current generation once: aborts the transport, reports the
    // failure to the handler and wakes every waiter.
    void fail(const client_transport_result& transport, std::exception_ptr failure = {}) noexcept;

    // The one client-initiated close of a running generation (RFC 9113
    // section 6.8: GOAWAY before closing). Stops admitting streams, queues
    // GOAWAY(NO_ERROR) and lets the writer fail the generation with `transport`
    // once every queued frame reached the transport, or after
    // one second. Streams still open are not cancelled
    // here; the bound ends them with the generation. Returns true while that
    // close is pending. Without a running generation that can send the frame
    // (or a timer to bound it) the generation fails at once and this returns
    // false.
    bool begin_close(const client_transport_result& transport) noexcept;

    // Requests a RST_STREAM (CANCEL unless the stream's input was malformed);
    // allocation failure fails the connection.
    void submit_reset(std::uint32_t stream_id, http2_error_code error = http2_error_code::cancel) noexcept;

    // Submits `bytes` on `stream_id` in DATA frames of at most `chunk_bytes`,
    // waiting for each batch to reach the transport and for send window.
    // An empty input ending the stream is still one DATA frame, sent only
    // while both send windows are positive (RFC 9113 section 6.9).
    // `stop()` is checked before each submission and after each wait. Input
    // beyond the declared Content-Length is rejected (invalid_message), as is
    // a stream that no longer sends (std::logic_error); other exceptions
    // propagate.
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
            if (submitted.status_ == http2_data_submit_status::closed ||
                submitted.status_ == http2_data_submit_status::invalid_message) {
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
    // send_data(). Rejected when the stream no longer sends (std::logic_error),
    // the content is incomplete or a trailer is invalid (invalid_message).
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

    [[nodiscard]] bool output_flushed() const noexcept;

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
    void reset() noexcept;

private:
    static constexpr std::size_t read_buffer_bytes = 16384;
    static constexpr std::size_t write_batch_bytes = 2 * http2_default_max_frame_size;

    [[nodiscard]] bool ended(std::uint64_t generation) const noexcept;

    void schedule_ping(std::uint64_t generation, std::chrono::steady_clock::time_point deadline);

    // Any received frame, the PING ACK included, proves liveness (as Go's
    // ReadIdleTimeout): one silent interval sends a PING, and the session
    // fails only when no input arrived during the interval after it. Input is
    // compared with when the PING was sent, not with the timer's firing time,
    // which may trail its deadline.
    void check_liveness(std::uint64_t generation) noexcept;

    void take_output_batch(std::pmr::string& output);

    [[nodiscard]] bool feed(std::string_view bytes_value);

    void drain();

    // Every input batch and completed write may close streams or apply peer
    // SETTINGS; only a grown request capacity wakes capacity waiters.
    void publish_state() noexcept;

    void fail(std::uint64_t generation, const client_transport_result& transport, std::exception_ptr failure) noexcept;

    void finish_task(std::uint64_t generation) noexcept;

    struct task_guard final {
        http2_client_session& session_;
        std::uint64_t generation_;
        ~task_guard();
    };

    [[nodiscard]] task<void> run_reader(std::uint64_t generation);

    [[nodiscard]] task<void> run_writer(std::uint64_t generation);

    http_client_connection& handler_;
    client_tcp_transport& transport_;
    task_scope& tasks_;
    const worker_handle& worker_;
    std::optional<timeout_duration> ping_interval_;
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
#endif

// One physical connection owner of an http_client_pool for one route: its
// transport, the HTTP/1 parse buffers and the HTTP/2 session with its stream
// registry. Reconnects keep the route. HTTP/1 exchanges run through
// http1_client_exchange; the pool owns admission, policy and reclamation.
// Worker-affine and address-stable.
class http_client_connection final {
public:
    enum class protocol_type : std::uint8_t { unknown,
        http1,
        http2 };

#ifdef RUVIA_ENABLE_HTTP2
    // One registered HTTP/2 request or push stream; a stack or driver record
    // that unregisters before its response state can be released.
    struct http2_stream final {
        http2_stream(const worker_handle& worker_value, http_client_response_state& state_value)
            : signal_(worker_value),
              state_(&state_value) {}
        http2_stream(worker_handle&&, http_client_response_state&) = delete;

        worker_signal signal_;
        http_client_response_state* state_;
        http_client_websocket_handshake* websocket_{};
        std::optional<std::error_code> error_;
        std::exception_ptr failure_;
        std::size_t registry_index_{0};
        std::uint64_t request_id_{0};
        std::uint64_t cancellation_id_{0};
        std::uint32_t stream_id_{0};
        // Borrowed from the request or push driver while registered.
        const operation_deadline* timeout_{nullptr};
        std::string_view request_target_;
        http_client_stream_timer timer_;
        bool complete_{false};
        // Caller-owned buffered suffix or its socket output is still pending.
        bool request_body_pending_{false};
        // The tunnel's final DATA frame entered session output. Its transport
        // completion can precede the upload coroutine's scheduled resumption.
        bool tunnel_end_submitted_{false};
        bool retryable_{false};
        // The peer refused the request with RST_STREAM(HTTP_1_1_REQUIRED)
        // before any response head (RFC 9113 section 7).
        bool http11_required_{false};
        // The request addresses the fixed origin, whose jar retains the
        // response's Set-Cookie fields.
        bool addresses_origin_{true};

        [[nodiscard]] bool failed() const noexcept {
            return error_.has_value() || failure_ != nullptr;
        }
    };
#endif

    // `server_name` is the TLS SNI and verified name (empty for http); `alpn`
    // is the TLS ALPN offer, and over plaintext http2 selects prior-knowledge
    // h2c, anything else HTTP/1.1.
    http_client_connection(http_client_pool& pool, std::size_t index, std::uint64_t serial,
        std::string_view dial_host, std::uint16_t dial_port, std::string_view server_name, bool alias_route,
        client_alpn alpn);
    void capture_connection_info(http_client_response_state& response, bool reused) const;
    void retire_addresses() noexcept;
    [[nodiscard]] std::uint64_t connection_id() const noexcept {
        return connection_id_;
    }
    ~http_client_connection();
    http_client_connection(const http_client_connection&) = delete;
    http_client_connection& operator=(const http_client_connection&) = delete;

    [[nodiscard]] std::size_t index() const noexcept {
        return index_;
    }
    [[nodiscard]] std::uint64_t serial() const noexcept {
        return serial_;
    }
    // The route is the alias default, authenticated for the alias origin.
    [[nodiscard]] bool alias_route() const noexcept {
        return alias_route_;
    }
    [[nodiscard]] bool serves(std::string_view dial_host, std::uint16_t dial_port, std::string_view server_name,
        client_alpn alpn) const noexcept;
    // Kept between uses: an HTTP/1 connection between exchanges, or an HTTP/2
    // session admitting streams without any.
    [[nodiscard]] bool idle() const noexcept;
    // Nothing runs on or borrows this owner any longer; the pool may destroy it.
    [[nodiscard]] bool reclaimable() const noexcept;
#ifdef RUVIA_ENABLE_HTTP2
    // Advisory only; the worker-affine request driver consumes the selected
    // capacity synchronously before another request can select it.
    [[nodiscard]] std::size_t available_http2_request_streams() const noexcept;
#endif
    // No transport, operation, session or stream uses this slot: a request
    // routed here connects at once.
    [[nodiscard]] bool idle_unconnected() const noexcept {
#ifdef RUVIA_ENABLE_HTTP2
        if (http2_.running() || !http2_streams_.empty()) {
            return false;
        }
#endif
        return !connected_ && http1_operations_ == 0;
    }

    // Serialized by connect_scheduler_. Returns true only when the existing
    // transport remains connected and is reused; a replacement returns false.
    // A previous HTTP/2 generation that is draining or still unwinding is
    // waited for first, within `acquire_timeout`.
    [[nodiscard]] task<bool> ensure_connected(
        const operation_deadline& timeout, const operation_deadline& acquire_timeout,
        stop_token stop_token_value, http_client_response_state& response);
    // Terminal close of the current connection; a running HTTP/2 session
    // fails through its single transition so every stream is woken.
    void close() noexcept;
    // Starts the idle bound of a connected HTTP/1 connection between exchanges
    // or an HTTP/2 connection without streams; its expiry closes the
    // connection unless it is in use again. A bound that cannot be armed
    // closes the connection at once. An idle HTTP/1 connection is also
    // watched: peer EOF, reset or unsolicited bytes close it at once.
    void begin_idle() noexcept;
    void end_idle() noexcept;
    // Closes a connection nothing uses; an HTTP/2 session sends GOAWAY first.
    void close_idle() noexcept;
    // Applies the retention policy to both buffers. Only between exchanges,
    // when no transport operation borrows their storage.
    void retain_idle_buffers() noexcept;
    // Pool shutdown: permanently refuses new leases, aborts the connect or
    // HTTP/1 exchange in progress with closing, then closes. An HTTP/2
    // session without streams closes with GOAWAY first.
    void shutdown() noexcept;
    // Lease cancellation: aborts the connect or HTTP/1 exchange in progress
    // with `reason` and closes the transport.
    void cancel(std::uint64_t generation, client_abort_reason reason) noexcept;
    // Stops the bound HTTP/1 response's output and wakes its producer waits.
    [[nodiscard]] bool cancel_by_id(std::uint64_t cancellation_id) noexcept;
    void abort_http1_response() noexcept;
    // Throws the HTTP client error of the transport's abort reason, if any.
    void throw_if_aborted() const;

#ifdef RUVIA_ENABLE_HTTP2
    // HTTP/2 (http2/http_client_http2.cpp). False when the multiplexed session
    // had ended (or the peer required HTTP/1.1, RFC 9113 section 7) and its
    // replacement connection speaks HTTP/1.1 instead: nothing was sent, and
    // the caller runs the request as an HTTP/1 exchange on that connection.
    // (ensure_connected() already rejects that outcome for an http2_only
    // client.) `acquire_timeout` bounds the first attempt's reconnect and
    // stream-capacity waits; a retry attempt gets its own acquisition bound.
    [[nodiscard]] task<bool> execute_http2(const http_client_request_storage& request,
        const operation_deadline& timeout, const operation_deadline& acquire_timeout, stop_token stop_token_value,
        http_client_response_state& state_value);
    // Registers a stream whose timeout_ is set and arms its deadline timer.
    void register_http2_stream(http2_stream& stream);
    void remove_http2_stream(http2_stream& stream) noexcept;
    void cancel_http2_stream(std::uint64_t request_id, client_abort_reason reason,
        http2_error_code error = http2_error_code::cancel) noexcept;
    [[nodiscard]] std::uint64_t next_http2_request_id() noexcept;
    void reprioritize(http_client_response_state& state_value, http_priority priority);
    // Wakes the writer to flush WINDOW_UPDATE for released DATA credit; the
    // stream's read inactivity bound restarts as the peer may send again.
    void release_response_data(http_client_response_state& state_value) noexcept;

    // http2_client_session handler.
    void on_http2_event(http2_event& event);
    [[nodiscard]] task<void> deliver_http2_informational(std::uint32_t stream_id);
    [[nodiscard]] std::optional<timeout_duration> http2_write_timeout() const noexcept;
    void on_http2_failure(const client_transport_result& transport, std::exception_ptr failure) noexcept;
    // http2_client_session handler: request-stream capacity grew, or the last
    // session task ended.
    void on_http2_capacity() noexcept;
    void on_http2_stopped() noexcept;
#endif

private:
    // The route's transport names; transport_ borrows them, so they precede it.
    std::pmr::string dial_host_;
    std::pmr::string server_name_;

public:
    client_tcp_transport transport_;
    http_io_buffer read_buffer_;
    http_io_buffer write_buffer_;
#ifdef RUVIA_ENABLE_HTTP2
    http2_client_session http2_;
#endif
    pool_lease_scheduler connect_scheduler_;
    pool_lease_scheduler http1_scheduler_;
    // HTTP/1 exchanges holding or waiting for http1_scheduler_.
    std::size_t http1_operations_{0};
    // Borrowed only while one serialized HTTP/1 exchange is active.
    http_client_response_state* http1_response_{nullptr};
    // Bumped around every connect and HTTP/1 exchange so stale cancellations
    // cannot reach a later operation.
    std::uint64_t generation_{0};
    std::uint64_t cancellation_id_{0};
    protocol_type protocol_{protocol_type::unknown};
    bool connected_{false};
    bool retired_{false};
    bool address_retired_{false};
    bool fresh_{false};
    // The connection key's offer; reconnects keep it.
    const client_alpn alpn_;
    // Requests the pool routed here that have not released it yet.
    std::size_t users_{0};
    // Order in which the connection last turned idle (larger is newer); 0
    // while it is in use or after it closed.
    std::uint64_t idle_order_{0};

private:
#ifdef RUVIA_ENABLE_HTTP2
    struct http2_state_waiter final {
        std::uint64_t cancellation_id_{};
        http2_state_waiter* next_{};
    };
    [[nodiscard]] http2_stream* find_http2_stream(std::uint32_t stream_id) noexcept;
    [[nodiscard]] http2_stream* find_http2_request(std::uint64_t request_id) noexcept;
    [[nodiscard]] bool http2_exchange_complete(const http2_stream& stream) noexcept;
    void fail_http2_stream(http2_stream& stream, std::exception_ptr failure, bool reset_stream) noexcept;
    // Wakes a stream's driver, and capacity waiters while it has no stream ID.
    void wake_http2_stream(http2_stream& stream) noexcept;
    // Peer input for the stream restarts its read inactivity bound.
    void restart_http2_stream_read(http2_stream& stream);
    // Arms timer_ for the earlier of the total and read deadlines.
    void schedule_http2_stream_timer(http2_stream& stream);
    void arm_http2_write_timer(worker_timer_registration& write_timer, const http2_stream& stream);
#endif
#ifdef RUVIA_ENABLE_HTTP2
    // Client close of a session that admits no further request: GOAWAY first
    // (RFC 9113 section 6.8), bounded by the session.
    void close_http2_session() noexcept;
#endif
    // Pingora's idle_poll: one watcher per connection, parked while the
    // connection is in use and retired with the pool.
    [[nodiscard]] task<void> watch_idle_http1();
#ifdef RUVIA_ENABLE_HTTP2
    void publish_http2_head(http2_stream& stream, http2_response_head_event& event);
    void consume_http2_data(std::uint32_t stream_id, std::string_view bytes, http2_received_data_credit credit);
    void finish_http2_message(std::uint32_t stream_id, http2_message_end_event& end);
    [[nodiscard]] task<bool> send_http2_request_data(http2_stream& stream, std::string_view bytes,
        http2_end_stream end_stream, const http_client_output_queue* upload = nullptr);
    [[nodiscard]] task<void> stream_http2_upload(const http_client_request_storage& request, http2_stream& stream,
        http_client_output_queue& upload);
    // The one wait for a previous HTTP/2 generation to retire: its session and
    // every stream that borrows its transport.
    [[nodiscard]] task<void> wait_for_http2_session_stop(
        const operation_deadline& timeout, stop_token stop_token_value, http_client_response_state& response);
#endif

    http_client_pool& pool_;
    std::size_t index_;
    std::uint64_t serial_;
    bool alias_route_;
    std::uint64_t connection_id_{};
    std::pmr::string peer_ip_{process_resource()};
#ifdef RUVIA_ENABLE_HTTP2
    std::pmr::vector<http2_stream*> http2_streams_;
    http2_stream* last_http2_stream_{nullptr};
    std::uint64_t next_http2_request_id_{0};
    // Request heads accepted by the current HTTP/2 generation: a later
    // request's stream runs on a reused connection.
    std::size_t http2_session_requests_{0};
    http2_state_waiter* state_waiters_{};
#endif
    // Idle watch of an HTTP/1 connection: wanted between begin_idle() and
    // end_idle(); each end bumps idle_period_ so a late readiness of an
    // earlier period is ignored.
    worker_signal idle_watch_signal_;
    std::uint64_t idle_period_{0};
    bool idle_watch_wanted_{false};
    // The watcher's readiness wait is pending and not yet cancelled.
    bool idle_wait_pending_{false};
    bool idle_watcher_started_{false};
    // Last: destroyed first, so its completion never sees a retiring connection.
    worker_timer_registration idle_timer_;
};

}  // namespace ruvia::detail
