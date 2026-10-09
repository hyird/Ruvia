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

#include "ruvia/core/connection_scanner.h"
#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/core/move_only_function.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_response_stream.h"
#include "ruvia/http/websocket_protocol_types.h"
#include "ruvia/web/detail/http/http_datagram_input.h"
#include "ruvia/web/streaming.h"

#include "context/context_services.h"
#include "context/http_connection_advertisement_output.h"
#include "context/http_interim_response_output.h"
#include "context/http_push_output.h"
#include "http/http_stream_read_result.h"
#include "http3/http3_buffered_response_output.h"
#include "http3/http3_sans_io_session_engine.h"
#include "http3/http3_stream_buffer.h"
#include "server/http_server_options.h"
#include "server/request_deadline.h"

namespace ruvia::detail {

class route_table;

struct http3_tunnel_callbacks final {
    void* context_{};
    bool (*attach_scanner_)(void*, std::uint64_t, connection_scanner::entry_type&) noexcept {};
    void (*output_ready_)(void*, std::uint64_t) noexcept {};
    void (*abort_)(void*, std::uint64_t) noexcept {};
    void (*input_consumed_)(void*) noexcept {};
    task<bool> (*push_)(void*, std::uint64_t, http_push_request_view){};
    void (*send_datagram_)(void*, std::uint64_t, std::span<const std::byte>){};
};

// Worker-affine owner for one HTTP/3 request or websocket tunnel. It
// pins the session request while routing, response preparation and bounded
// publication run; it does not own or expose the session stream's private memory.
//
// RFC 9114 §§4.2.2, 7.2.4 and 10.5.1 say SHOULD NOT: the peer setting is
// advisory. Compare the decoded sum(name + value + 32), including :status, not
// QPACK/frame bytes. This dispatch checks before its first local buffer publication;
// after any HEADERS prefix is handed off it must finish that same field section
// even if a later peer setting is lower. This does not guarantee a final
// transport-level field-section limit.
//
// The connection owner must retire/cancel inbound delivery separately through
// http3_server_stream_input. publish_step() never waits for buffer capacity: the
// connection-level scheduler owns fairness and local capacity recovery.
// context_services is snapshotted by value; its worker/token/capability and
// connection-metadata borrows, and all other constructor references, must
// outlive this owner and its tasks.
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
        deadline,
    };

    // DATA/CONTROL identify the required buffer lane. Local demands are
    // handled by publish_step() without waiting for buffer capacity.
    enum class publication_demand_type : std::uint8_t {
        data,
        control,
        local_complete,
        local_cancelled,
        local_buffer_stopped,
        local_peer_limit_rejected,
        local_failed,
        not_ready,
        wrong_worker,
    };

    struct publish_result_type final {
        publish_status_type status_{publish_status_type::not_ready};
        std::size_t bytes_published_{};
        // Non-None only for backpressure; identifies the capacity lane, not cursor state.
        publish_block_reason_type block_reason_{publish_block_reason_type::none};
    };

    http3_buffered_request_dispatch(http3_sans_io_session_engine& session_value, const route_table& routes_value,
        worker_memory& worker_value, context_services services, const http_server_options& options,
        http3_stream_buffer& outbound, http3_stream_id message_id,
        connection_scanner::entry_type& scanner_entry, asio::any_io_executor executor,
        http3_tunnel_callbacks tunnel_callbacks, std::uint64_t response_prelude_bytes = 0);
    ~http3_buffered_request_dispatch();
    http3_buffered_request_dispatch(const http3_buffered_request_dispatch&) = delete;
    http3_buffered_request_dispatch& operator=(const http3_buffered_request_dispatch&) = delete;
    http3_buffered_request_dispatch(http3_buffered_request_dispatch&&) = delete;
    http3_buffered_request_dispatch& operator=(http3_buffered_request_dispatch&&) = delete;

    // Both tasks are lazy. The first actual start acquires the request lease;
    // constructing and discarding either cold task changes no session state.
    [[nodiscard]] std::variant<http3_streaming_response_head, http3_response_head_failure> encode_streaming_response_head(http_response response, http_known_method method, http_response_stream_kind kind, http_response_trailer_intent trailers) {
        return session_.encode_streaming_response_head(message_id_.stream_id_, std::move(response), method, kind, trailers);
    }
    [[nodiscard]] std::variant<http3_response_field_section, http3_response_head_failure> encode_response_trailers(std::span<const http3_field_section_field_view> fields_value) {
        return session_.encode_response_trailers(message_id_.stream_id_, fields_value);
    }
    [[nodiscard]] task<prepare_status_type> prepare() &;
    task<prepare_status_type> prepare() && = delete;
    [[nodiscard]] task<run_status_type> run_handler() &;
    task<run_status_type> run_handler() && = delete;

    // Pure, worker-affine query. A foreign worker gets wrong_worker before any
    // dispatch/session/buffer state is inspected. Does not publish, acknowledge,
    // allocate/free, invoke handlers, change state, or
    // release resources.
    [[nodiscard]] publication_demand_type publication_demand() const noexcept;

    // Copies at most one cursor segment (clipped to one buffer block) per
    // call. Only successful try_send/try_send_control operations are acknowledged.
    // Backpressure identifies the capacity lane without arming a wait here.
    // Local terminal demands are committed and cleaned up here, never by the query.
    [[nodiscard]] publish_result_type publish_step() & noexcept;
    publish_result_type publish_step() && = delete;

    // During publication only, subscribe the supplied allocation-free callback
    // to an explicitly armed request deadline. No registration is made for the
    // default no-deadline case. The callback is synchronous on the stop source's
    // requesting thread; connection owners require that source to be worker-affine.
    [[nodiscard]] bool register_publication_deadline_callback(
        move_only_function<void()> callback) &;
    bool register_publication_deadline_callback(move_only_function<void()>) && = delete;

    // The reason is latched before request_deadline_ is destroyed, so owners can
    // distinguish deadline cancellation from worker stop and explicit cancel.
    [[nodiscard]] cancellation_reason_type cancellation_reason() const noexcept;

    // Worker-affine. Marks output terminal before requesting stop. The caller
    // must separately retire inbound delivery and await/join a running handler.
    void cancel() & noexcept;
    void cancel() && = delete;

    [[nodiscard]] task<std::error_code> publish_tunnel_handshake(
        std::span<const char> headers_frame);
    void commit_final_response() {
        interim_output_.commit_final();
    }
    [[nodiscard]] task<void> publish_response_frame(std::uint64_t type, std::span<const char> payload);
    [[nodiscard]] task<void> publish_response_bytes(std::span<const char> bytes);
    [[nodiscard]] task<void> finish_response();
    [[nodiscard]] bool response_aborted() const noexcept {
        return cancellation_requested() || tunnel_aborted_ || peer_field_section_rejected_;
    }
    [[nodiscard]] bool response_field_section_allowed(std::size_t decoded_size) const noexcept;
    [[noreturn]] void reject_peer_field_section();
    void notify_tunnel_input() noexcept;
    [[nodiscard]] task<http_stream_read_result> read_tunnel(std::pmr::string& buffer);
    [[nodiscard]] task<std::optional<http_datagram_input>> read_datagram_input();
    [[nodiscard]] http_datagram_session_config datagram_config() const {
        return session_.datagram_config(message_id_.stream_id_);
    }
    void send_datagram(std::span<const std::byte> bytes);
    [[nodiscard]] task<std::error_code> write_tunnel(std::string_view bytes,
        http_stream_end disposition);
    [[nodiscard]] task<bool> wait_tunnel_receive_end();
    void abort_tunnel() noexcept;
    [[nodiscard]] asio::any_io_executor executor() const noexcept {
        return executor_;
    }

    [[nodiscard]] bool handler_active() const noexcept;
    [[nodiscard]] bool response_ready() const noexcept;
    // True once all response bytes and the final FIN control are enqueued in
    // outbound_; this is handoff completion, not transport write or peer receipt.
    [[nodiscard]] bool complete() const noexcept;
    // Cumulative HTTP/3 wire bytes successfully handed to outbound_.
    [[nodiscard]] std::uint64_t published_wire_bytes() const noexcept;
    [[nodiscard]] std::exception_ptr failure() const noexcept;

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

    using websocket_dispatch_result_type = std::variant<run_status_type, http_response>;

    [[nodiscard]] task<run_status_type> run_handler_inner();
    [[nodiscard]] task<websocket_dispatch_result_type> run_websocket_handler();
    [[nodiscard]] task<std::optional<http_response>> run_tunnel_handler();
    [[nodiscard]] task<void> write_interim_response(const http_interim_response_head& head);
    [[nodiscard]] task<void> await_response_publication();
    [[nodiscard]] task<run_status_type> write_buffered_after_interim(http_buffered_response_write_plan plan);
    [[nodiscard]] task<run_status_type> write_file_response(http_buffered_response_write_plan plan);
    [[nodiscard]] task<std::optional<std::span<const std::byte>>> read_request_body();
    [[nodiscard]] task<void> drain_request_body();
    [[nodiscard]] bool on_worker() const noexcept;
    void notify_tunnel_output() noexcept;
    [[nodiscard]] publish_result_type publish_stream_step(publication_demand_type demand) noexcept;
    [[nodiscard]] bool cancellation_requested() const noexcept;
    void latch_cancellation_reason() const noexcept;
    [[nodiscard]] bool exceeds_peer_field_section_limit() const noexcept;
    void fail(std::exception_ptr failure = {}) noexcept;
    void release_dispatch_storage() noexcept;
    static void peer_transport_fin_timeout_tick(void* target, std::int64_t now_ms) noexcept;
    void arm_peer_transport_fin_timeout() noexcept;
    void disarm_peer_transport_fin_timeout() noexcept;

    http3_sans_io_session_engine& session_;
    const route_table& routes_;
    worker_memory& worker_;
    const context_services services_;
    const http_server_options& options_;
    http3_stream_buffer& outbound_;
    const http3_stream_id message_id_;
    connection_scanner::entry_type& scanner_entry_;
    connection_scanner::periodic_check_registration_type peer_transport_fin_check_;
    asio::any_io_executor executor_;
    const http3_tunnel_callbacks tunnel_callbacks_;
    std::pmr::string active_request_body_;
    std::optional<body_reader> request_body_reader_;
    worker_signal tunnel_input_available_;
    worker_signal tunnel_output_available_;

    // Declaration order makes destruction output/cursor -> response -> request
    // arena -> lease. The lease is returned only after every dependent object dies.
    std::optional<http3_sans_io_session_engine::request_lease_type> lease_;
    std::optional<request_memory> request_memory_;
    std::optional<http_response> response_;
    std::optional<http3_buffered_response_output> output_;
    std::pmr::string stream_frame_;
    std::size_t stream_frame_offset_{};
    std::pmr::string tunnel_data_frame_;
    std::uint64_t stream_published_wire_bytes_{};
    bool peer_field_section_rejected_{};
    bool stream_output_active_{};
    bool tunnel_data_pending_{};
    bool tunnel_established_control_pending_{};
    bool tunnel_established_control_published_{};
    bool tunnel_fin_pending_{};
    bool stream_output_ended_{};
    std::chrono::milliseconds peer_transport_fin_timeout_{};
    std::int64_t peer_transport_fin_deadline_ms_{};
    bool peer_fin_timeout_armed_{};
    bool tunnel_aborted_{};

    stop_source request_stop_source_;
    stop_token combined_worker_and_request_stop_;
    stop_registration tunnel_stop_registration_;
    stop_registration publication_deadline_registration_;
    std::optional<request_deadline> request_deadline_;
    std::optional<context_services> request_services_;

    std::exception_ptr failure_;
    std::uint64_t published_wire_bytes_{};
    state_type state_{state_type::cold};
    mutable cancellation_reason_type cancellation_reason_{cancellation_reason_type::none};
    bool handler_active_{false};
    bool cancellation_requested_{false};
    bool deadline_armed_{};
    bool publication_deadline_callback_registered_{};
    http_interim_response_output interim_output_;
    http_connection_advertisement_output connection_advertisements_;
    http_push_output push_output_;
};

}  // namespace ruvia::detail
