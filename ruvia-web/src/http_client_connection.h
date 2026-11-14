#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "client_tcp_transport.h"
#include "client_tls.h"
#include "http2_client_session.h"
#include "http_client_request_storage.h"
#include "http_client_response_state.h"
#include "http_io_buffer.h"
#include "http_priority.h"
#include "ip_address.h"
#include "operation_options.h"
#include "pool_lease.h"
#include "stop_token.h"
#include "task.h"
#include "timeout_duration.h"
#include "worker_ref.h"
#include "worker_signal.h"
#include "worker_timer.h"

#ifdef RUVIA_ENABLE_HTTP2
#include "http2_connection.h"
#include "http2_frame.h"
#endif

namespace ruvia::detail {

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
        http2_stream(const worker_ref& worker_value, http_client_response_state& state_value) :
            signal_(worker_value),
            state_(&state_value) {}
        http2_stream(worker_ref&&, http_client_response_state&) = delete;

        worker_signal signal_;
        http_client_response_state* state_;
        http_client_websocket_handshake* websocket_{};
        std::optional<std::error_code> error_;
        std::exception_ptr failure_;
        std::size_t registry_index_{0};
        std::uint64_t request_id_{0};
        std::uint64_t cancellation_id_{0};
        // The session's input_reads() when the head was submitted or the push
        // promised. A stream timeout without a read since then evicts the
        // session instead of only resetting the stream.
        std::uint64_t input_mark_{0};
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
    http_client_connection(http_client_pool& pool, std::size_t index, std::uint64_t serial, std::string_view dial_host, std::uint16_t dial_port, std::string_view server_name, bool alias_route, client_alpn alpn);
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
    [[nodiscard]] bool serves(std::string_view dial_host, std::uint16_t dial_port, std::string_view server_name, client_alpn alpn) const noexcept;
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
    [[nodiscard]] task<bool> ensure_connected(const operation_deadline& timeout, const operation_deadline& acquire_timeout, stop_token stop_token_value, http_client_response_state& response);
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
    // HTTP/2 (http2_client.cpp). False when the multiplexed session
    // had ended (or the peer required HTTP/1.1, RFC 9113 section 7) and its
    // replacement connection speaks HTTP/1.1 instead: nothing was sent, and
    // the caller runs the request as an HTTP/1 exchange on that connection.
    // (ensure_connected() already rejects that outcome for an http2_only
    // client.) `acquire_timeout` bounds the first attempt's reconnect and
    // stream-capacity waits; a retry attempt gets its own acquisition bound.
    [[nodiscard]] task<bool> execute_http2(const http_client_request_storage& request, const operation_deadline& timeout, const operation_deadline& acquire_timeout, stop_token stop_token_value, http_client_response_state& state_value);
    // Registers a stream whose timeout_ is set and arms its deadline timer.
    void register_http2_stream(http2_stream& stream);
    void remove_http2_stream(http2_stream& stream) noexcept;
    // Ends a stream with RST_STREAM. A timeout of a stream whose head was
    // submitted fails the whole session instead when the session read nothing
    // since then (see http2_stream::input_mark_).
    void cancel_http2_stream(std::uint64_t request_id, client_abort_reason reason, http2_error_code error = http2_error_code::cancel) noexcept;
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
    std::string dial_host_;
    std::string server_name_;

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
    [[nodiscard]] task<bool> send_http2_request_data(http2_stream& stream, std::string_view bytes, http2_end_stream end_stream, const http_client_output_queue* upload = nullptr);
    [[nodiscard]] task<void> stream_http2_upload(const http_client_request_storage& request, http2_stream& stream, http_client_output_queue& upload);
    // The one wait for a previous HTTP/2 generation to retire: its session and
    // every stream that borrows its transport.
    [[nodiscard]] task<void> wait_for_http2_session_stop(const operation_deadline& timeout, stop_token stop_token_value, http_client_response_state& response);
#endif

    http_client_pool& pool_;
    std::size_t index_;
    std::uint64_t serial_;
    bool alias_route_;
    std::uint64_t connection_id_{};
    ip_address peer_ip_{};
#ifdef RUVIA_ENABLE_HTTP2
    std::vector<http2_stream*> http2_streams_;
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
