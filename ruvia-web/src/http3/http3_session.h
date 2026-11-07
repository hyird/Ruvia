#pragma once

#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <string_view>
#include <system_error>
#include <unordered_map>

#include <asio/ip/udp.hpp>
#include <openssl/ssl.h>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/task.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/core/worker_timer.h"
#include "ruvia/http/http3_quic_connection.h"

#include "context/context_services.h"
#include "http3/http3_connection_id.h"
#include "http3/http3_response_write.h"
#include "http3/http3_stream.h"
#include "server/connection_count.h"
#include "server/connection_scanner.h"
#include "server/http_request_admission.h"
#include "server/inbound_buffer_resource.h"
#include "tls/tls_served_certificate.h"

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
        std::optional<ngtcp2_cid> retry_scid = std::nullopt,
        std::span<const std::uint8_t> retry_token = {});
    ~http3_session();

    http3_session(const http3_session&) = delete;
    http3_session& operator=(const http3_session&) = delete;
    http3_session(http3_session&&) = delete;
    http3_session& operator=(http3_session&&) = delete;

    // One datagram for this connection, from `remote`.
    void receive(std::span<const std::byte> datagram, const asio::ip::udp::endpoint& remote) noexcept;
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
    // Returns receive flow-control credit for request content the application
    // consumed (RFC 9000 §4.1).
    void consume_stream_data(std::int64_t stream_id, std::size_t bytes) noexcept;
    [[nodiscard]] const worker_handle& worker() const noexcept;
    [[nodiscard]] std::pmr::memory_resource* worker_resource() const noexcept;
    [[nodiscard]] std::string_view remote_address() const& noexcept RUVIA_LIFETIMEBOUND {
        return remote_address_;
    }
    std::string_view remote_address() const&& = delete;
    [[nodiscard]] http3_quic_status submit_datagram(std::int64_t stream_id, std::span<const std::byte> payload);

private:
    using stream_pointer = std::unique_ptr<http3_stream, pmr_object_deleter<http3_stream>>;

    enum class lifecycle_type : std::uint8_t {
        open,
        // The QUIC connection closed (either side, or idle): late packets
        // are absorbed until the closing period elapses.
        closing,
        ended,
    };

    struct free_tls final {
        void operator()(SSL* tls) const noexcept;
    };
    struct free_crypto final {
        void operator()(ngtcp2_crypto_ossl_ctx* crypto) const noexcept;
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
    void on_headers(http3_headers_event& event);
    void on_request_head(http3_headers_event& event);
    void on_data(const http3_data_event& event);
    void on_datagram(http3_datagram_event& event);
    void on_stream_end(std::int64_t stream_id);
    void on_stream_abort(std::int64_t stream_id) noexcept;
    void on_stream_close(const http3_stream_close_event& event) noexcept;
    void wake_output_waiters() noexcept;

    // Requests.
    [[nodiscard]] bool admit_stream(std::int64_t stream_id);
    [[nodiscard]] task<void> dispatch_one(std::int64_t stream_id);
    [[nodiscard]] task<http_response> respond_body_failure(const http_request& request,
        request_memory& request_memory_value, http_request_body_failure failure,
        const context_services& request_services, http_response_tail& tail);
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
    asio::ip::udp::endpoint local_;
    asio::ip::udp::endpoint remote_;
    // The sender of the datagram being read, for its ngtcp2 path.
    asio::ip::udp::endpoint path_remote_;
    std::pmr::string remote_address_;
    // Keys this connection holds in the endpoint's table.
    std::pmr::vector<http3_cid_key> connection_ids_;
    std::unique_ptr<SSL, free_tls> tls_;
    std::array<unsigned char, 32> tls_resumption_context_{};
    std::unique_ptr<ngtcp2_crypto_ossl_ctx, free_crypto> crypto_;
    ngtcp2_crypto_conn_ref connection_reference_{};
    // Destroyed first: ngtcp2 still references the TLS objects above.
    std::optional<http3_quic_connection> connection_;
    http3_quic_datagram datagram_;
    std::pmr::string client_certificate_;
    std::optional<tls_served_certificate> served_certificate_;
    inbound_buffer_resource inbound_buffers_;
    std::optional<context_services> base_services_;
    http_request_admission_policy admission_policy_;
    http3_termination termination_;
    worker_signal write_signal_;
    worker_signal handler_finished_;
    // Declared after the inbound buffers their bodies borrow.
    std::pmr::unordered_map<std::int64_t, stream_pointer> streams_;
    http3_buffered_response_writer buffered_response_writer_;
    std::size_t active_handler_tasks_{0};
    std::size_t accepted_request_heads_{0};
    std::size_t tunnel_streams_{0};
    lifecycle_type lifecycle_{lifecycle_type::open};
    bool drain_begun_{false};
    // A connection timeout started (or joined) the drain: its time bound ends
    // the connection instead of only making the GOAWAY boundary final.
    bool timeout_drain_{false};
    bool drain_expired_{false};
    // The deadline the expiry timer is armed for, in ngtcp2 time.
    ngtcp2_tstamp armed_expiry_{UINT64_MAX};
    connection_scanner::entry scanner_entry_;
    std::optional<connection_scanner::guard> scanner_guard_;
    connection_scanner::periodic_check_registration scanner_checks_;
    // Declared last: unregistered before the state their completions touch.
    worker_timer_registration drain_timer_;
    worker_timer_registration timer_;
};

}  // namespace ruvia::detail
