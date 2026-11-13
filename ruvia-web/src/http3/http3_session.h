#pragma once

#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>

#include <array>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>

#include <asio/ip/udp.hpp>
#include <openssl/ssl.h>

#include "ruvia/core/stop_token.h"
#include "ruvia/core/task.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/core/worker_timer.h"
#include "ruvia/http/http3_connection.h"
#include "ruvia/http/http3_quic_connection.h"
#include "ruvia/http/http3_request_head.h"
#include "ruvia/http/http_priority.h"
#include "ruvia/http/http_request_body_error.h"
#include "ruvia/http/http_request_trailers.h"
#include "ruvia/http/protocol_byte_limit.h"
#include "ruvia/web/detail/callback_ref.h"

#include "context/context_services.h"
#include "http3/http3_connection_id.h"
#include "http3/http3_quic_runtime.h"
#include "router/route_resolution.h"
#include "server/inbound_buffer_budget.h"
#include "server/multiplexed_stream_signal.h"

// Web-side state of one HTTP/3 request stream. The QUIC/HTTP/3 core owns
// framing, QPACK and flow control; this runtime owns what a route needs: the
// validated head, the request body as the route consumes it (buffered or
// streamed, with flow-control credit returned only as bytes are consumed), the
// trailers, and the wake/stop signal a dispatched handler waits on.

#include "ruvia/http/http_status.h"

#include "server/connection_count.h"
#include "server/connection_lifecycle.h"
#include "server/connection_scanner.h"
#include "server/http_request_admission.h"
#include "server/http_server_tls_contexts.h"
#include "server/response_file_source.h"
#include "tls/tls_served_certificate.h"

namespace ruvia::detail {

enum class http3_body_store_status : std::uint8_t {
    stored,
    too_large,
    backlog_overflow,
    buffer_limit,
};

// Tunnel bytes taken together with the receive credit they hold; the reader
// drops the credit once it consumed the bytes.
struct http3_tunnel_chunk final {
    std::string bytes_;
    std::optional<http3_received_data_credit> credit_;
};

class http3_stream final {
public:
    http3_stream(http3_request_head head, route_resolution resolution, request_body_mode body_mode,
        inbound_buffer_account* inbound = nullptr) noexcept
        : head_(std::move(head)),
          resolution_(std::move(resolution)),
          body_mode_(body_mode),
          body_reservation_(inbound),
          active_reservation_(inbound) {}

    http3_stream(const http3_stream&) = delete;
    http3_stream& operator=(const http3_stream&) = delete;

    [[nodiscard]] std::int64_t id() const noexcept {
        return head_.stream_id();
    }
    [[nodiscard]] const http3_request_head& head() const& noexcept RUVIA_LIFETIMEBOUND {
        return head_;
    }
    const http3_request_head& head() const&& = delete;
    [[nodiscard]] const route_resolution& resolution() const& noexcept RUVIA_LIFETIMEBOUND {
        return resolution_;
    }
    const route_resolution& resolution() const&& = delete;
    [[nodiscard]] request_body_mode body_mode() const noexcept {
        return body_mode_;
    }
    [[nodiscard]] http_request_trailers& trailers() & noexcept RUVIA_LIFETIMEBOUND {
        return trailers_;
    }
    http_request_trailers& trailers() && = delete;
    [[nodiscard]] const std::optional<http_priority>& priority() const& noexcept RUVIA_LIFETIMEBOUND {
        return priority_;
    }
    const std::optional<http_priority>& priority() const&& = delete;
    void set_priority(std::optional<http_priority> priority) noexcept {
        priority_ = priority;
    }
    void defer_dispatch() noexcept {
        dispatch_pending_ = true;
    }
    [[nodiscard]] bool dispatch_pending() const noexcept {
        return dispatch_pending_ && !dispatched();
    }
    void defer_continue() noexcept {
        continue_pending_ = true;
    }
    [[nodiscard]] bool take_continue_pending() noexcept {
        return std::exchange(continue_pending_, false);
    }

    void store_datagram(std::string_view payload, std::size_t byte_limit) {
        if ((datagrams_ && datagrams_->size() >= 64) || datagram_bytes_ > byte_limit ||
            payload.size() > byte_limit - datagram_bytes_) {
            return;  // RFC 9221 permits loss; never backpressure reliable DATA.
        }
        try {
            if (!datagrams_) {
                datagrams_.emplace();
            }
            datagrams_->emplace_back(payload);
            datagram_bytes_ += payload.size();
        } catch (const inbound_buffer_limit_error&) {
            return;
        }
        if (signal_) {
            signal_->wake();
        }
    }
    [[nodiscard]] std::optional<std::string> pop_datagram() {
        if (!datagrams_ || datagrams_->empty()) {
            return std::nullopt;
        }
        auto payload = std::move(datagrams_->front());
        datagrams_->pop_front();
        datagram_bytes_ -= payload.size();
        return payload;
    }
    [[nodiscard]] http3_tunnel_chunk take_tunnel_bytes() {
        http3_tunnel_chunk result{std::string{}, std::exchange(body_credit_, std::nullopt)};
        result.bytes_.swap(body_);
        body_reservation_.clear();
        return result;
    }

    // Buffered body: the whole content, borrowed by the request at dispatch;
    // its credit returns as it is stored. Streaming body and tunnel: the bytes
    // queued for the reader, holding their credit until it takes them.
    [[nodiscard]] http3_body_store_status store(std::string_view data, http3_received_data_credit&& credit,
        protocol_byte_limit total_limit, std::size_t backlog_limit) {
        if (http_request_body_addition_failure(received_bytes_, data.size(), total_limit).has_value()) {
            return http3_body_store_status::too_large;
        }
        if (body_mode_ == request_body_mode::stream &&
            (body_.size() > backlog_limit || data.size() > backlog_limit - body_.size())) {
            return http3_body_store_status::backlog_overflow;
        }
        try {
            inbound_append(body_, body_reservation_, data);
        } catch (const inbound_buffer_limit_error&) {
            return http3_body_store_status::buffer_limit;
        }
        received_bytes_ += data.size();
        if (body_mode_ == request_body_mode::stream && credit.valid()) {
            if (body_credit_) {
                body_credit_->merge(std::move(credit));
            } else {
                body_credit_.emplace(std::move(credit));
            }
        }
        return http3_body_store_status::stored;
    }

    [[nodiscard]] std::string_view buffered_body() const& noexcept RUVIA_LIFETIMEBOUND {
        return body_;
    }
    std::string_view buffered_body() const&& = delete;

    // Streaming reader: hands out every queued byte as one chunk, valid until
    // the next pop(). The previous chunk's flow-control credit returns now;
    // true when it held any, so the caller can let the writer send it.
    [[nodiscard]] std::pair<std::string_view, bool> pop() & RUVIA_LIFETIMEBOUND {
        const bool returned = active_credit_.has_value();
        active_credit_.reset();
        if (body_credit_) {
            active_credit_.emplace(std::move(*body_credit_));
            body_credit_.reset();
        }
        inbound_clear_retaining_small(active_chunk_, active_reservation_);
        active_chunk_.swap(body_);
        active_reservation_.swap(body_reservation_);
        return {active_chunk_, returned};
    }
    std::pair<std::string_view, bool> pop() && = delete;

    // The handler finished: the content's storage goes back to the
    // connection's inbound budget, and credit for content received but never
    // read returns so a stopped reader cannot strand connection-level credit.
    // True when any credit returned.
    bool discard_body() noexcept {
        const bool returned = body_credit_.has_value() || active_credit_.has_value();
        body_credit_.reset();
        active_credit_.reset();
        inbound_discard(body_, body_reservation_);
        inbound_discard(active_chunk_, active_reservation_);
        datagrams_.reset();
        datagram_bytes_ = 0;
        return returned;
    }

    void mark_body_failure(http_request_body_failure failure) noexcept {
        if (!body_failure_.has_value()) {
            body_failure_ = failure;
        }
    }
    [[nodiscard]] const std::optional<http_request_body_failure>& body_failure() const& noexcept RUVIA_LIFETIMEBOUND {
        return body_failure_;
    }
    const std::optional<http_request_body_failure>& body_failure() const&& = delete;

    // The request content ended (the peer's FIN, or no content at all).
    void mark_request_ended() noexcept {
        request_ended_ = true;
        request_body_deadline_.close();
    }
    [[nodiscard]] bool request_ended() const noexcept {
        return request_ended_;
    }

    void mark_request_body_open(std::int64_t now_ms) noexcept {
        request_body_deadline_.open(now_ms);
    }
    void note_request_body_progress(std::int64_t now_ms) noexcept {
        request_body_deadline_.progress(now_ms);
    }
    void mark_request_body_closed() noexcept {
        request_body_deadline_.close();
    }
    // The request body is still open and went `inactivity` without DATA, or
    // `completion` since it opened (request_body_timeout_ and
    // body_completion_timeout_).
    [[nodiscard]] bool request_body_overdue(std::int64_t now_ms,
        const std::optional<timeout_duration>& inactivity,
        const std::optional<timeout_duration>& completion) const noexcept {
        return request_body_deadline_.overdue(now_ms, inactivity, completion);
    }

    void mark_tunnel() noexcept {
        tunnel_ = true;
    }
    [[nodiscard]] bool tunnel() const noexcept {
        return tunnel_;
    }

    void mark_response_committed() noexcept {
        response_committed_ = true;
    }
    [[nodiscard]] bool response_committed() const noexcept {
        return response_committed_;
    }

    // The peer reset the stream, or this side did: no response byte may follow.
    void mark_aborted() noexcept {
        aborted_ = true;
        if (signal_) {
            signal_->stop();
        }
    }
    [[nodiscard]] bool aborted() const noexcept {
        return aborted_;
    }

    // The QUIC stream closed in both directions.
    void mark_closed() noexcept {
        closed_ = true;
    }
    [[nodiscard]] bool closed() const noexcept {
        return closed_;
    }

    [[nodiscard]] multiplexed_stream_signal* begin_dispatch(
        const worker_ref& worker_value, multiplexed_session_termination& termination) {
        if (signal_ || handler_done_) {
            return nullptr;
        }
        signal_.emplace(worker_value, termination);
        if (aborted_) {
            signal_->stop();
        }
        return &*signal_;
    }
    [[nodiscard]] bool dispatched() const noexcept {
        return signal_.has_value();
    }
    [[nodiscard]] multiplexed_stream_signal* signal() & noexcept RUVIA_LIFETIMEBOUND {
        return signal_ ? &*signal_ : nullptr;
    }
    multiplexed_stream_signal* signal() && = delete;
    [[nodiscard]] const multiplexed_stream_signal* signal() const& noexcept RUVIA_LIFETIMEBOUND {
        return signal_ ? &*signal_ : nullptr;
    }
    const multiplexed_stream_signal* signal() const&& = delete;

    void mark_handler_done() noexcept {
        handler_done_ = true;
    }
    [[nodiscard]] bool handler_done() const noexcept {
        return handler_done_;
    }

private:
    http3_request_head head_;
    route_resolution resolution_;
    request_body_mode body_mode_;
    http_request_trailers trailers_;
    std::optional<http_priority> priority_{};
    // Received request content: the whole body (buffered), or the queue the
    // reader drains (streaming). active_chunk_ backs the last popped view.
    std::string body_;
    inbound_buffer_reservation body_reservation_;
    std::string active_chunk_;
    inbound_buffer_reservation active_reservation_;
    // Receive credit of body_ and active_chunk_ (streaming body or tunnel).
    std::optional<http3_received_data_credit> body_credit_;
    std::optional<http3_received_data_credit> active_credit_;
    std::optional<std::deque<std::string>> datagrams_;
    std::size_t datagram_bytes_{0};
    std::size_t received_bytes_{0};
    std::optional<http_request_body_failure> body_failure_{};
    std::optional<multiplexed_stream_signal> signal_;
    multiplexed_request_body_deadline request_body_deadline_;
    bool request_ended_{false};
    bool tunnel_{false};
    bool response_committed_{false};
    bool aborted_{false};
    bool closed_{false};
    bool handler_done_{false};
    bool dispatch_pending_{false};
    bool continue_pending_{false};
};

}  // namespace ruvia::detail

namespace ruvia {
class http_response;
class http_buffered_response_write_plan;
}  // namespace ruvia

namespace ruvia::detail {

class http3_session;
class http3_stream;
enum class http3_data_write_status : std::uint8_t {
    completed,
    // The stream was reset (by either side) or stopped.
    stream_aborted,
    // The connection ended.
    terminated,
};

// Submits all of `bytes` as DATA of a stream, at most the stream's output
// budget at a time (http3_connection::submit_data): when the budget is
// spent it waits for the peer's acknowledgements, timed by the session
// against write_timeout_. A Content-Length overrun throws std::length_error.
[[nodiscard]] task<http3_data_write_status> write_http3_stream_data(
    http3_session& session, std::int64_t stream_id, multiplexed_stream_signal& signal, std::string_view bytes);

// Writes a buffered or file response on one request stream: the head with its
// write plan's Content-Length, then the content within the stream's output
// budget. A file body is opened and validated before the head without being
// read; each later chunk is read only once output budget is free, at most that
// much, while holding one of multiplexed_file_read_slots through its submission. A
// stream it cannot complete is reset through the session.
class http3_buffered_response_writer final {
public:
    http3_buffered_response_writer(http3_session& session, response_file_io file_io) noexcept;

    http3_buffered_response_writer(const http3_buffered_response_writer&) = delete;
    http3_buffered_response_writer& operator=(const http3_buffered_response_writer&) = delete;
    http3_buffered_response_writer(http3_buffered_response_writer&&) = delete;
    http3_buffered_response_writer& operator=(http3_buffered_response_writer&&) = delete;

    [[nodiscard]] task<multiplexed_buffered_response_write_result> write(std::int64_t stream_id,
        const http_response& response, http_buffered_response_write_plan write_plan);

private:
    [[nodiscard]] task<multiplexed_buffered_response_write_result> write_file_body(
        std::int64_t stream_id, multiplexed_stream_signal& signal, response_file_source& source,
        http_status_code committed_status);
    [[nodiscard]] bool stream_ended(std::int64_t stream_id) noexcept;

    http3_session& session_;
    response_file_io file_io_;
    multiplexed_file_read_limiter file_reads_;
};

}  // namespace ruvia::detail

struct ngtcp2_crypto_ossl_ctx;

namespace ruvia::detail {

class http3_endpoint;
struct http3_worker_services;

// Per-stream DATA a connection holds between submission and the peer's
// acknowledgement (http3_connection_options::max_stream_output_bytes_).
inline constexpr std::size_t http3_stream_output_bytes = 256 * 1024;
// Largest UDP payload this server sends: fits a 1500-byte Ethernet MTU behind
// IPv6, UDP and common tunnel headers, without path MTU discovery.
inline constexpr std::size_t http3_max_datagram_bytes = 1350;

// One HTTP/3 connection on the worker that accepted it: its TLS session, its
// QUIC/HTTP/3 state (http3_quic_connection), its one QUIC timer, its writer
// and the dispatch of its request streams through the shared request spine
// (admission, routes, response tail), as the HTTP/2 session engine does.
//
// The endpoint feeds datagrams in with receive(); run() is the connection's
// task: it writes every datagram the connection produces, re-arms the timer to
// expiry(), and returns once the connection has ended and every handler has
// finished. All of it runs on the owning worker.
//
// Lifecycle: open -> (graceful drain on the request cap or a connection
// timeout: shutdown-notice GOAWAY, a bounded round trip, final GOAWAY,
// CONNECTION_CLOSE(H3_NO_ERROR) once no stream is left) -> closing period
// (RFC 9000 §10.2, three probe timeouts absorbing late packets) -> ended. A
// worker stop, eviction or a timeout past its grace ends it at once with
// CONNECTION_CLOSE(H3_NO_ERROR).
class http3_session final {
public:
    // Accepts the connection the client Initial `initial` opens (RFC 9000
    // §7.2): a server connection ID owned by this worker, the client's chosen
    // ID as original_destination_connection_id.
    http3_session(http3_endpoint& endpoint, connection_slot slot, const ngtcp2_pkt_hd& initial,
        const asio::ip::udp::endpoint& remote, const ngtcp2_cid& original_dcid,
        std::optional<ngtcp2_cid> retry_scid, std::span<const std::uint8_t> retry_token,
        connection_lifecycle lifecycle);
    ~http3_session();

    http3_session(const http3_session&) = delete;
    http3_session& operator=(const http3_session&) = delete;
    http3_session(http3_session&&) = delete;
    http3_session& operator=(http3_session&&) = delete;

    // One datagram for this connection, from `remote`.
    void receive(std::span<const std::byte> datagram, const asio::ip::udp::endpoint& remote,
        const asio::ip::udp::endpoint& local, std::uint32_t interface_index) noexcept;
    [[nodiscard]] task<void> run();
    // Ends the connection now: CONNECTION_CLOSE(H3_NO_ERROR), streams
    // cancelled, no closing period. The scanner's close action.
    void close_now() noexcept;

    // Stream I/O for the response writers, the streaming sink and the request
    // body reader of this connection's handlers.
    [[nodiscard]] http3_connection& codec() noexcept {
        return connection_->http3();
    }
    [[nodiscard]] http3_stream* find_stream(std::int64_t stream_id) noexcept;
    // Queues the stream's output for the writer.
    void wake_writer() noexcept;
    // Abandons both directions of a request stream (RFC 9114 §4.1.1) and
    // stops every wait of its handler.
    void reset_stream(std::int64_t stream_id, http3_error_code error) noexcept;
    [[nodiscard]] const worker_ref& worker() const noexcept;
    [[nodiscard]] std::string_view remote_address() const& noexcept RUVIA_LIFETIMEBOUND {
        return remote_address_;
    }
    std::string_view remote_address() const&& = delete;
    [[nodiscard]] http3_quic_status submit_datagram(std::int64_t stream_id, std::span<const std::byte> payload);

private:
    using stream_pointer = std::unique_ptr<http3_stream>;

    enum class lifecycle_type : std::uint8_t {
        open,
        // The QUIC connection closed (either side, or idle): late packets
        // are absorbed until the closing period elapses.
        closing,
        ended,
    };

    static ngtcp2_conn* native_connection(ngtcp2_crypto_conn_ref* reference) noexcept;
    static int new_connection_id(ngtcp2_conn*, ngtcp2_cid* cid, ngtcp2_stateless_reset_token* token,
        std::size_t size, void* user_data) noexcept;
    static int remove_connection_id(ngtcp2_conn*, const ngtcp2_cid* cid, void* user_data) noexcept;

    [[nodiscard]] static ngtcp2_tstamp now() noexcept;
    [[nodiscard]] ngtcp2_path path(asio::ip::udp::endpoint& remote) noexcept;
    void register_connection_id(const ngtcp2_cid& cid);
    void unregister_connection_ids() noexcept;

    // Connection events.
    void process_events() noexcept;
    void drain_events();
    void start_application();
    void on_request_head(http3_request_head_event& event);
    void on_data(http3_data_event& event);
    void on_datagram(const http3_datagram_event& event);
    void on_message_end(const http3_message_end_event& event);
    void on_stream_end(std::int64_t stream_id);
    void on_stream_abort(std::int64_t stream_id) noexcept;
    void on_stream_close(const http3_stream_close_event& event) noexcept;
    void wake_output_waiters() noexcept;

    // Requests.
    [[nodiscard]] bool admit_stream(std::int64_t stream_id);
    [[nodiscard]] task<void> dispatch_one(std::int64_t stream_id);
    void finish_stream_handler(std::int64_t stream_id) noexcept;
    void remove_stream_if_done(std::int64_t stream_id) noexcept;
    void set_inactivity_phase() noexcept;
    void expire_stream_deadlines(std::int64_t now_ms) noexcept;

    // Output and timers.
    [[nodiscard]] task<void> flush();
    [[nodiscard]] task<void> send_datagram();
    void arm_timer() noexcept;
    void timer_fired(worker_timer_outcome outcome) noexcept;
    void enter_closing() noexcept;
    void terminate(std::error_code error) noexcept;

    // Graceful drain (RFC 9114 §5.2).
    void request_drain() noexcept;
    void begin_drain() noexcept;
    void arm_drain_timer() noexcept;
    void drain_timer_expired() noexcept;
    void finish_drain_if_ready() noexcept;

    http3_endpoint& endpoint_;
    const http3_worker_services& services_;
    connection_slot slot_;
    connection_lifecycle connection_lifecycle_;
    // conn_info::connection_id() of this connection.
    std::uint64_t connection_id_;
    asio::ip::udp::endpoint local_;
    // The actual destination address of the Initial, including wildcard-bound
    // listeners. TLS identity selection borrows this stable address.
    asio::ip::address tls_local_address_;
    asio::ip::udp::endpoint remote_;
    // The sender of the datagram being read, for its ngtcp2 path.
    asio::ip::udp::endpoint path_remote_;
    asio::ip::udp::endpoint path_local_;
    std::uint32_t path_interface_index_;
    std::string remote_address_;
    // Present only with an on_tls_client_hello observer; tls_ borrows it, so
    // it is declared before tls_.
    std::string local_address_;
    std::optional<tls_client_hello_observation> client_hello_;
    // Keys this connection holds in the endpoint's table.
    std::vector<http3_cid_key> connection_ids_;
    std::unique_ptr<SSL, free_tls> tls_;
    std::array<unsigned char, 32> tls_resumption_context_{};
    std::unique_ptr<ngtcp2_crypto_ossl_ctx, free_crypto> crypto_;
    ngtcp2_crypto_conn_ref connection_reference_{};
    // Destroyed first: ngtcp2 still references the TLS objects above.
    std::optional<http3_quic_connection> connection_;
    http3_quic_datagram datagram_;
    std::string client_certificate_;
    std::optional<tls_served_certificate> served_certificate_;
    inbound_buffer_account inbound_buffers_;
    std::optional<context_services> base_services_;
    http_request_admission_policy admission_policy_;
    multiplexed_session_termination termination_;
    worker_signal write_signal_;
    worker_signal handler_finished_;
    // Declared after the inbound buffers their bodies borrow.
    std::unordered_map<std::int64_t, stream_pointer> streams_;
    http3_buffered_response_writer buffered_response_writer_;
    std::size_t active_handler_tasks_{0};
    std::size_t accepted_request_heads_{0};
    std::size_t tunnel_streams_{0};
    lifecycle_type lifecycle_{lifecycle_type::open};
    bool drain_begun_{false};
    // The deadline the expiry timer is armed for, in ngtcp2 time.
    ngtcp2_tstamp armed_expiry_{UINT64_MAX};
    connection_scanner::entry scanner_entry_;
    std::optional<connection_scanner::guard> scanner_guard_;
    connection_scanner::periodic_check_registration scanner_checks_;
    // Declared last: unregistered before the state their completions touch.
    multiplexed_drain_timer drain_timer_;
    worker_timer_registration timer_;
};

}  // namespace ruvia::detail
