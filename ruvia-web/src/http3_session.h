#pragma once

#if defined(RUVIA_ENABLE_HTTP3)

#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_ossl.h>

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
#include <vector>

#include <asio/ip/udp.hpp>
#include <openssl/ssl.h>

#include "connection_lifecycle.h"
#include "connection_scanner.h"
#include "context_access.h"
#include "http3_endpoint.h"
#include "http3_quic_connection.h"
#include "http3_quic_driver.h"
#include "http_request_body_failure.h"
#include "inbound_buffer.h"
#include "multiplexed_session.h"
#include "response_file.h"
#include "route_table.h"
#include "server_listener.h"
#include "stop_token.h"
#include "streaming.h"
#include "task.h"
#include "tls_server.h"
#include "worker_ref.h"
#include "worker_signal.h"
#include "worker_timer.h"

// One HTTP/3 connection on the worker that accepted it: its request streams,
// buffered response writer and QUIC/HTTP/3 session driver.
namespace ruvia::detail {

// Web-side state of one HTTP/3 request stream. The QUIC/HTTP/3 core owns
// framing, QPACK and flow control; to the request stream state shared with
// HTTP/2 this adds what HTTP/3 alone needs: the validated head it owns, a
// tunnel's stored HTTP Datagrams, dispatch deferred until the handshake
// completed (0-RTT), and the abort/close lifecycle of the QUIC stream.
class http3_stream final : public multiplexed_request_stream_state {
public:
    http3_stream(http3_request_head head, route_resolution resolution, request_body_mode body_mode, inbound_buffer_account* inbound = nullptr) noexcept :
        multiplexed_request_stream_state(inbound),
        head_(std::move(head)),
        datagram_reservation_(inbound) {
        (void)select_route(std::move(resolution), body_mode);
    }

    [[nodiscard]] std::int64_t id() const noexcept {
        return head_.stream_id();
    }
    [[nodiscard]] const http3_request_head& head() const& noexcept RUVIA_LIFETIMEBOUND {
        return head_;
    }
    const http3_request_head& head() const&& = delete;
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

    // HTTP Datagrams of a tunnel (RFC 9297) waiting for its reader, their
    // payload bytes charged to the inbound budget like body bytes. One that
    // does not fit -- the per-stream count, `byte_limit` or the budget -- is
    // dropped: RFC 9221 permits loss, and a datagram must never backpressure
    // reliable DATA.
    void store_datagram(std::string_view payload, std::size_t byte_limit) {
        if ((datagrams_ && datagrams_->size() >= 64) || datagram_bytes_ > byte_limit || payload.size() > byte_limit - datagram_bytes_) {
            return;
        }
        try {
            datagram_reservation_.resize_to(datagram_bytes_ + payload.size());
            if (!datagrams_) {
                datagrams_.emplace();
            }
            datagrams_->emplace_back(payload);
        } catch (const inbound_buffer_limit_error&) {
            // Thrown by the charge itself: nothing was reserved.
            return;
        } catch (...) {
            datagram_reservation_.shrink_to(datagram_bytes_);
            throw;
        }
        datagram_bytes_ += payload.size();
        if (auto* stream_signal = signal()) {
            stream_signal->wake();
        }
    }
    // The reader owns the payload from here on.
    [[nodiscard]] std::optional<std::string> pop_datagram() {
        if (!datagrams_ || datagrams_->empty()) {
            return std::nullopt;
        }
        auto payload = std::move(datagrams_->front());
        datagrams_->pop_front();
        datagram_bytes_ -= payload.size();
        datagram_reservation_.shrink_to(datagram_bytes_);
        return payload;
    }

    // The handler finished or the stream was abandoned: body content and
    // stored datagrams go back to the inbound budget. True when receive
    // credit returned with them.
    bool discard_inbound() noexcept {
        datagrams_.reset();
        datagram_bytes_ = 0;
        datagram_reservation_.clear();
        return discard_body();
    }

    // The request content ended (the peer's FIN, or no content at all).
    void mark_request_ended() noexcept {
        body_queue().mark_ended();
        mark_request_body_closed();
    }
    [[nodiscard]] bool request_ended() const noexcept {
        return body_queue().ended();
    }

    // The peer reset the stream, or this side did: no response byte may follow.
    void mark_aborted() noexcept {
        aborted_ = true;
        if (auto* stream_signal = signal()) {
            stream_signal->stop();
        }
    }
    [[nodiscard]] bool aborted() const noexcept {
        return aborted_;
    }
    // Counts the stream once toward the Rapid-Reset budget: false when the
    // peer's other direction already did.
    [[nodiscard]] bool note_peer_abandoned() noexcept {
        return !std::exchange(peer_abandoned_, true);
    }

    // The QUIC stream closed in both directions.
    void mark_closed() noexcept {
        closed_ = true;
    }
    [[nodiscard]] bool closed() const noexcept {
        return closed_;
    }

    void mark_handler_done() noexcept {
        handler_done_ = true;
    }
    [[nodiscard]] bool handler_done() const noexcept {
        return handler_done_;
    }

private:
    http3_request_head head_;
    std::optional<std::deque<std::string>> datagrams_;
    inbound_buffer_reservation datagram_reservation_;
    std::size_t datagram_bytes_{0};
    bool aborted_{false};
    bool closed_{false};
    bool peer_abandoned_{false};
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
[[nodiscard]] task<http3_data_write_status> write_http3_stream_data(http3_session& session, std::int64_t stream_id, multiplexed_stream_signal& signal, std::string_view bytes);

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

    [[nodiscard]] task<multiplexed_buffered_response_write_result> write(std::int64_t stream_id, const http_response& response, http_buffered_response_write_plan write_plan);

private:
    [[nodiscard]] task<multiplexed_buffered_response_write_result> write_file_body(std::int64_t stream_id, multiplexed_stream_signal& signal, response_file_source& source, http_status_code committed_status);
    [[nodiscard]] bool stream_ended(std::int64_t stream_id) noexcept;

    http3_session& session_;
    response_file_io file_io_;
    multiplexed_file_read_limiter file_reads_;
};

}  // namespace ruvia::detail

struct ngtcp2_crypto_ossl_ctx;

namespace ruvia::detail {

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
    http3_session(http3_endpoint& endpoint, connection_slot slot, const ngtcp2_pkt_hd& initial, const asio::ip::udp::endpoint& remote, const ngtcp2_cid& original_dcid, std::optional<ngtcp2_cid> retry_scid, std::span<const std::uint8_t> retry_token, connection_lifecycle lifecycle);
    ~http3_session();

    http3_session(const http3_session&) = delete;
    http3_session& operator=(const http3_session&) = delete;
    http3_session(http3_session&&) = delete;
    http3_session& operator=(http3_session&&) = delete;

    // One datagram for this connection, from `remote`.
    void receive(std::span<const std::byte> datagram, const asio::ip::udp::endpoint& remote, const asio::ip::udp::endpoint& local, std::uint32_t interface_index) noexcept;
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
    static int new_connection_id(ngtcp2_conn*, ngtcp2_cid* cid, ngtcp2_stateless_reset_token* token, std::size_t size, void* user_data) noexcept;
    static int remove_connection_id(ngtcp2_conn*, const ngtcp2_cid* cid, void* user_data) noexcept;

    [[nodiscard]] static ngtcp2_tstamp now() noexcept;
    [[nodiscard]] ngtcp2_path path(asio::ip::udp::endpoint& remote) noexcept;
    void register_connection_id(const ngtcp2_cid& cid);
    void unregister_connection_ids() noexcept;

    // Connection events.
    void drain_events() noexcept;
    void dispatch_events();
    void start_application();
    void on_request_head(http3_request_head_event& event);
    void on_data(http3_data_event& event);
    void on_datagram(const http3_datagram_event& event);
    void on_message_end(const http3_message_end_event& event);
    void on_stream_end(std::int64_t stream_id);
    void on_stream_abort(const http3_transport_action_event& event) noexcept;
    void on_stream_close(const http3_stream_closed_event& event) noexcept;
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
    // Present only with a tls_client_hello_ observer; tls_ borrows it, so
    // it is declared before tls_.
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
    // Rapid Reset (CVE-2023-44487), as HTTP/2 counts it: request streams the
    // peer abandoned against responses this connection completed.
    std::uint64_t peer_reset_streams_{0};
    std::uint64_t completed_responses_{0};
    std::size_t tunnel_streams_{0};
    lifecycle_type lifecycle_{lifecycle_type::open};
    bool drain_begun_{false};
    // A writer pass completed after the final GOAWAY was queued, so the
    // drain's CONNECTION_CLOSE no longer overtakes it.
    bool final_goaway_flushed_{false};
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

#endif
