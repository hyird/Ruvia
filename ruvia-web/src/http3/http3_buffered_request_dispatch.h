#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <span>
#include <string_view>
#include <system_error>
#include <variant>

#include <asio/any_io_executor.hpp>

#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/core/move_only_function.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/http/http3_data_write_plan.h"
#include "ruvia/http/http3_headers_frame.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_response_stream.h"
#include "ruvia/web/detail/http/http_datagram_input.h"
#include "ruvia/web/streaming.h"

#include "body/http_request_body_facade.h"
#include "context/context_services.h"
#include "context/http_connection_advertisement_output.h"
#include "context/http_interim_response_output.h"
#include "context/http_push_output.h"
#include "http/http_stream_read_result.h"
#include "http3/http3_buffered_response_output.h"
#include "http3/http3_sans_io_session_engine.h"
#include "server/connection_scanner.h"
#include "server/http_request_admission.h"
#include "server/http_server_options.h"
#include "server/response_file_source.h"

namespace ruvia::detail {

class route_table;
class http3_server_connection;

// Worker-affine owner for one HTTP/3 request or tunnel, owned by its
// http3_server_connection. It pins the session request while admission,
// routing, response preparation and publication run; it does not own or
// expose the session stream's private memory.
//
// Response bytes are published straight from their owners -- encoded HEADERS
// frames, DATA frame headers planned by http3_data_write_plan and borrowed
// payloads -- to the connection's QUIC output. A suspended writer keeps its
// bytes alive until QUIC accepted all of them; publish_step() never waits.
//
// RFC 9114 §§4.2.2, 7.2.4 and 10.5.1 say SHOULD NOT: the peer setting is
// advisory. Compare the decoded sum(name + value + 32), including :status, not
// QPACK/frame bytes. This dispatch checks before its first publication; after
// any HEADERS prefix is accepted it must finish that same field section even if
// a later peer setting is lower.
//
// The connection owner retires/cancels inbound delivery separately through
// http3_server_stream_input and owns fairness across streams.
class http3_buffered_request_dispatch final {
public:
    enum class prepare_status_type : std::uint8_t {
        prepared,
        already_prepared,
        request_unavailable,
        wrong_worker,
        cancelled,
        failed,
    };

    enum class run_status_type : std::uint8_t {
        response_ready,
        already_run,
        wrong_worker,
        cancelled,
        file_payload_unsupported,
        peer_field_section_limit,
        tunnel_complete,
        output_complete,
        failed,
    };

    enum class publish_status_type : std::uint8_t {
        bytes_published,
        control_published,
        fin_published,
        backpressured,
        complete,
        not_ready,
        wrong_worker,
        cancelled,
        peer_limit_rejected,
        failed,
    };

    enum class publish_block_reason_type : std::uint8_t {
        none,
        data,
        control,
    };

    enum class cancellation_reason_type : std::uint8_t {
        none,
        worker_stop,
        explicit_value,
    };

    // DATA/CONTROL name the QUIC operation the next step performs (stream
    // bytes, or FIN/tunnel establishment). Local demands are handled by
    // publish_step() without touching QUIC.
    enum class publication_demand_type : std::uint8_t {
        data,
        control,
        local_complete,
        local_cancelled,
        local_peer_limit_rejected,
        local_failed,
        not_ready,
        wrong_worker,
    };

    struct publish_result_type final {
        publish_status_type status_{publish_status_type::not_ready};
        // Non-None only for backpressure; identifies the parked lane.
        publish_block_reason_type block_reason_{publish_block_reason_type::none};
    };

    http3_buffered_request_dispatch(http3_server_connection& owner, std::uint64_t stream_id,
        std::optional<std::uint64_t> push_id, bool received_early_data,
        connection_scanner::entry_type& scanner_entry);
    ~http3_buffered_request_dispatch();
    http3_buffered_request_dispatch(const http3_buffered_request_dispatch&) = delete;
    http3_buffered_request_dispatch& operator=(const http3_buffered_request_dispatch&) = delete;
    http3_buffered_request_dispatch(http3_buffered_request_dispatch&&) = delete;
    http3_buffered_request_dispatch& operator=(http3_buffered_request_dispatch&&) = delete;

    [[nodiscard]] std::variant<http3_streaming_response_head, http3_response_head_failure> encode_streaming_response_head(
        http_response response, http_known_method method, http_response_stream_kind kind, http_response_trailer_intent trailers) {
        return session_.encode_streaming_response_head(stream_id_, std::move(response), method, kind, trailers);
    }
    [[nodiscard]] std::variant<http3_headers_frame, http3_response_head_failure> encode_response_trailers(
        std::span<const http3_field_section_field_view> fields_value) {
        return session_.encode_response_trailers(stream_id_, fields_value);
    }
    // Both tasks are lazy. The first actual start acquires the request lease;
    // constructing and discarding either cold task changes no session state.
    [[nodiscard]] task<prepare_status_type> prepare() &;
    task<prepare_status_type> prepare() && = delete;
    [[nodiscard]] task<run_status_type> run_handler() &;
    task<run_status_type> run_handler() && = delete;

    // Pure, worker-affine query: never publishes, allocates or changes state.
    [[nodiscard]] publication_demand_type publication_demand() const noexcept;

    // Writes as much of the current output as QUIC accepts, or performs the
    // pending FIN/tunnel establishment. Backpressure names the lane to park
    // on. Local terminal demands are committed and cleaned up here.
    [[nodiscard]] publish_result_type publish_step() & noexcept;
    publish_result_type publish_step() && = delete;

    [[nodiscard]] cancellation_reason_type cancellation_reason() const noexcept;

    // Worker-affine. Marks output terminal before requesting stop. The caller
    // must separately retire inbound delivery and await/join a running handler.
    void cancel() & noexcept;
    void cancel() && = delete;

    void commit_final_response() {
        interim_output_.commit_final();
    }
    // Each publication completes once QUIC accepted every byte; the frame and
    // payload are borrowed until then.
    [[nodiscard]] task<void> publish_headers(const http3_headers_frame& frame);
    [[nodiscard]] task<void> publish_data(const http3_data_write_plan::chunk_type& chunk);
    [[nodiscard]] task<void> publish_response_bytes(std::span<const char> bytes);
    [[nodiscard]] task<void> finish_response();
    [[nodiscard]] bool response_aborted() const noexcept {
        return cancellation_requested() || tunnel_aborted_ || peer_field_section_rejected_;
    }
    [[nodiscard]] bool response_field_section_allowed(std::size_t decoded_size) const noexcept;
    [[noreturn]] void reject_peer_field_section();

    // Tunnel transport (http3_tunnel_dispatch.cpp).
    void notify_tunnel_input() noexcept;
    template <typename buffer_type>
    [[nodiscard]] task<http_stream_read_result> read_tunnel(buffer_type& buffer);
    [[nodiscard]] task<std::optional<http_datagram_input>> read_datagram_input();
    [[nodiscard]] http_datagram_session_config datagram_config() const {
        return session_.datagram_config(stream_id_);
    }
    void send_datagram(std::span<const std::byte> bytes);
    [[nodiscard]] task<std::error_code> write_tunnel(std::string_view bytes, http_stream_end disposition);
    void abort_tunnel() noexcept;
    [[nodiscard]] asio::any_io_executor executor() const noexcept;

    [[nodiscard]] bool handler_active() const noexcept {
        return handler_active_;
    }
    [[nodiscard]] bool response_ready() const noexcept {
        return state_ == state_type::output_ready || state_ == state_type::publishing;
    }
    // True once QUIC accepted every response byte and the FIN; this is not
    // peer receipt.
    [[nodiscard]] bool complete() const noexcept {
        return state_ == state_type::complete;
    }

private:
    enum class state_type : std::uint8_t {
        cold,
        preparing,
        prepared,
        running,
        output_ready,
        publishing,
        complete,
        cancelled,
        peer_limit_rejected,
        failed,
    };

    using upgrade_result_type = std::variant<run_status_type, http_response>;

    [[nodiscard]] task<run_status_type> run_handler_inner();
    [[nodiscard]] task<run_status_type> write_buffered_after_interim(http_buffered_response_write_plan plan);
    [[nodiscard]] task<run_status_type> write_file_response(response_file_source& source,
        response_body_chunk chunk, http_buffered_response_write_plan plan);
    [[nodiscard]] task<void> write_interim_response(const http_interim_response_head& head);
    [[nodiscard]] task<bool> await_output();
    [[nodiscard]] task<void> publish_output();
    [[nodiscard]] task<std::optional<std::span<const std::byte>>> read_request_body();
    [[nodiscard]] task<void> drain_request_body();
    [[nodiscard]] publish_result_type publish_stream_step(publication_demand_type demand) noexcept;
    [[nodiscard]] bool output_pending() const noexcept {
        return !pending_head_.empty() || !pending_body_.empty();
    }
    void record_access(http_status_code status) const noexcept;
    [[nodiscard]] bool on_worker() const noexcept;
    void notify_output() noexcept;
    [[nodiscard]] bool cancellation_requested() const noexcept;
    void latch_cancellation_reason() const noexcept;
    [[nodiscard]] bool exceeds_peer_field_section_limit() const noexcept;
    void fail(std::exception_ptr failure = {}) noexcept;
    void release_dispatch_storage() noexcept;

    // Tunnels and WebSocket (http3_tunnel_dispatch.cpp).
    [[nodiscard]] task<upgrade_result_type> run_tunnel_handler(const context_services& services);
    [[nodiscard]] task<upgrade_result_type> run_websocket_handler(const context_services& services);
    [[nodiscard]] task<std::error_code> publish_tunnel_handshake(std::span<const char> headers_frame,
        http3_data_write_plan data_plan);
    [[nodiscard]] task<bool> wait_tunnel_receive_end();
    static void peer_transport_fin_timeout_tick(void* target, std::int64_t now_ms) noexcept;
    void arm_peer_transport_fin_timeout() noexcept;
    void disarm_peer_transport_fin_timeout() noexcept;

    http3_server_connection& owner_;
    http3_sans_io_session_engine& session_;
    const route_table& routes_;
    worker_memory& worker_;
    const context_services services_;
    const http_server_options& options_;
    const std::uint64_t stream_id_;
    const std::optional<std::uint64_t> push_id_;
    const bool received_early_data_;
    const std::chrono::steady_clock::time_point start_;
    connection_scanner::entry_type& scanner_entry_;
    connection_scanner::periodic_check_registration_type peer_transport_fin_check_;
    std::pmr::string active_request_body_;
    struct request_reader final {
        http3_buffered_request_dispatch& owner_;
        task<std::optional<std::span<const std::byte>>> read() {
            return owner_.read_request_body();
        }
    };
    std::optional<body_reader_binding<request_reader>> request_body_reader_;
    worker_signal tunnel_input_available_;
    worker_signal tunnel_output_available_;

    // Declaration order makes destruction output/cursor -> response -> request
    // arena -> lease. The lease is returned only after every dependent object dies.
    std::optional<http3_sans_io_session_engine::request_lease_type> lease_;
    std::optional<request_memory> request_memory_;
    std::optional<http_response> response_;
    std::optional<http3_buffered_response_output> output_;
    // Tunnel DATA framing, planned from the 2xx CONNECT response.
    std::optional<http3_data_write_plan> tunnel_data_plan_;
    // Borrowed output of the suspended writer: a frame (or frame header) and
    // its payload. Both stay alive until QUIC accepted every byte.
    std::span<const char> pending_head_{};
    std::span<const char> pending_body_{};
    bool peer_field_section_rejected_{};
    bool stream_output_active_{};
    bool output_published_{};
    bool tunnel_establish_pending_{};
    bool tunnel_established_{};
    bool fin_pending_{};
    bool stream_output_ended_{};
    std::chrono::milliseconds peer_transport_fin_timeout_{};
    std::int64_t peer_transport_fin_deadline_ms_{};
    bool peer_fin_timeout_armed_{};
    bool tunnel_aborted_{};

    stop_source request_stop_source_;
    stop_token combined_worker_and_request_stop_;
    stop_registration tunnel_stop_registration_;
    std::optional<context_services> request_services_;
    // Borrows the request lease and request services.
    std::optional<http_request_admission> admission_;

    std::exception_ptr failure_;
    state_type state_{state_type::cold};
    mutable cancellation_reason_type cancellation_reason_{cancellation_reason_type::none};
    bool handler_active_{false};
    bool cancellation_requested_{false};
    http_interim_response_output interim_output_;
    http_connection_advertisement_output connection_advertisements_;
    http_push_output push_output_;
};

}  // namespace ruvia::detail
