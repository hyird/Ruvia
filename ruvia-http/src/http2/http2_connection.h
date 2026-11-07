#pragma once

// HTTP/2 sans-I/O connection core.
//
// A pure protocol state machine: it never touches a socket, a coroutine, a timer,
// or asio. Inbound bytes go to one nghttp2_session (nghttp2_session_mem_recv2),
// which owns framing, HPACK, stream states, flow control and SETTINGS/PING/GOAWAY
// handling; its callbacks feed Ruvia's message semantics (field validation,
// content accounting, CONNECT/tunnel phases, leases) and publish typed events.
// Every operation drains nghttp2_session_mem_send2 eagerly into the
// frame-segmented output buffer, so the owner sees whole frames through
// take_output_batch(). The I/O loop, concurrency model and timeouts live entirely
// in the caller.
//
// nghttp2 is a C library: exceptions never cross its frames. A callback that
// throws records the exception and fails the nghttp2 call; the connection is
// then failed and the exception rethrown once nghttp2 has returned. Allocation
// failure inside nghttp2 is likewise fatal for the connection.

#include <nghttp2/nghttp2.h>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/http/detail/server/http_response_trailers.h"
#include "ruvia/http/detail/util/borrowed_view.h"
#include "ruvia/http/http2_request_content.h"
#include "ruvia/http/http2_types.h"
#include "ruvia/http/http_client.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_interim_response.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_response_server.h"
#include "ruvia/http/http_response_stream.h"
#include "ruvia/http/websocket_handshake.h"

#include "http2/http2_event.h"
#include "http2/http2_field_list.h"
#include "http2/http2_local_connection_state.h"
#include "http2/http2_local_settings.h"
#include "http2/http2_nghttp2_memory.h"
#include "http2/http2_output_buffer.h"
#include "http2/http2_request_headers.h"
#include "http2/http2_stream_lease_ledger.h"
#include "http2/http2_stream_state.h"
#include "http2/http2_stream_table.h"
#include "response/http_framed_response_head_plan.h"
#include "response/http_response_content_semantics.h"
#include "websocket/websocket_server_negotiation.h"

namespace ruvia::detail {

// An ORIGIN or ALTSVC payload handed to nghttp2_submit_extension(). It lives on
// the submitting call's stack; nghttp2 packs it while that call drains the
// session, and the single in-tree payload encoder writes exactly size_ bytes.
struct http2_extension_payload final {
    std::size_t size_{0};
    void (*write_)(const http2_extension_payload&, std::span<char>) noexcept {nullptr};
    std::span<const std::string_view> origins_{};
    std::string_view origin_{};
    std::string_view field_value_{};
};

class http2_connection final {
public:
    explicit http2_connection(std::pmr::memory_resource* resource, http2_role role = http2_role::server,
        bool enable_push = false, bool receive_origin_advertisements = false, bool receive_frame_metadata = false);
    ~http2_connection();
    http2_connection(const http2_connection&) = delete;
    http2_connection& operator=(const http2_connection&) = delete;

    [[nodiscard]] http2_role role() const noexcept {
        return role_;
    }

    // --- inbound ---------------------------------------------------------------
    // Feed raw bytes read from the peer. nghttp2 consumes the whole span and
    // buffers any incomplete frame itself. Calling while events remain returns
    // events_pending and consumes nothing; drain peek_event()/consume_event()
    // first. protocol_failure is terminal. DATA events borrow the accepted span
    // until the next consuming feed().
    [[nodiscard]] http2_feed_result feed(std::string_view in);
    template <http_temporary_owning_char_string input_type>
    http2_feed_result feed(input_type&&) = delete;
    // The owner materializes the front event before consuming it, so an exception
    // never consumes the only protocol notification for a stream.
    [[nodiscard]] http2_event* peek_event() & noexcept;
    [[nodiscard]] http2_event* peek_event() && = delete;
    void consume_event() noexcept;

    // Access an assembled request head / stream for the owner to build an http_request.
    [[nodiscard]] http2_stream_state* stream(std::uint32_t stream_id) & noexcept;
    [[nodiscard]] http2_stream_state* stream(std::uint32_t) && = delete;
    // New outbound work is closed after a fatal connection error. Owner cleanup
    // uses the raw stream lookup so outstanding leases remain releasable.
    [[nodiscard]] http2_stream_state* find_outbound_stream(std::uint32_t stream_id) noexcept;
    [[nodiscard]] http2_stream_receive_status stream_receive_status(
        std::uint32_t stream_id) const noexcept;
    [[nodiscard]] std::optional<http2_server_request_view> server_request_view(
        std::uint32_t stream_id) const noexcept;

    // --- outbound --------------------------------------------------------------
    [[nodiscard]] http2_output_batch_result take_output_batch(std::size_t max_bytes,
        std::pmr::string& into, http2_data_output_observer_type observer, void* observer_context);
    [[nodiscard]] bool wants_write() const noexcept {
        return output_.wants_write();
    }

    // A stale/mismatched plan is rejected distinctly before any field or stream
    // mutation. Informational status codes and HTTP/2-unrepresentable control
    // semantics are rejected as well.
    [[nodiscard]] std::variant<http_buffered_response_write_plan, http2_response_head_submit_error>
    submit_response_head(std::uint32_t stream_id,
        const http_response& response, http_buffered_response_write_plan write_plan);
    [[nodiscard]] std::variant<http_response_stream_commit_plan, http2_response_head_submit_error>
    submit_streaming_response_head(
        std::uint32_t stream_id, http_response head,
        http_response_trailer_intent trailer_intent);
    [[nodiscard]] http2_data_submit_result submit_data(std::uint32_t stream_id,
        std::string_view chunk, http2_end_stream end_stream, std::size_t max_payload_bytes);
    [[nodiscard]] http2_submit_status submit_interim_response_head(
        std::uint32_t stream_id, const http_interim_response_head& response);
    [[nodiscard]] std::variant<websocket_extended_connect_negotiation, http2_websocket_handshake_submit_error>
    submit_websocket_handshake(
        std::uint32_t stream_id, const http_request& request,
        const websocket_handshake_validation_result& validation,
        websocket_server_handshake_options options);
    [[nodiscard]] http2_submit_status submit_connect_response_head(
        std::uint32_t stream_id, const http_response& response);
    [[nodiscard]] std::variant<std::uint32_t, http2_push_submit_error> submit_push_promise(
        std::uint32_t associated_stream_id, http_push_request_view request);
    [[nodiscard]] http2_finish_status finish_request(std::uint32_t stream_id,
        std::span<const http_header_view> trailers = {});
    [[nodiscard]] http2_finish_status finish_response(
        std::uint32_t stream_id, const http_response_trailer_section& trailers);
    [[nodiscard]] http2_submit_status submit_origin_advertisement(std::span<const std::string_view> origins);
    [[nodiscard]] http2_submit_status submit_alternative_service_advertisement(std::uint32_t stream_id,
        std::string_view origin, std::string_view field_value);
    [[nodiscard]] http2_submit_status submit_priority_update(std::uint32_t stream_id, http_priority_fields fields);

    [[nodiscard]] http2_submit_status submit_reset(std::uint32_t stream_id, http2_error_code error);
    [[nodiscard]] http2_submit_status submit_ping();
    [[nodiscard]] http2_submit_status submit_max_concurrent_streams(std::uint32_t limit);

    // --- lifecycle -------------------------------------------------------------
    // A connection error is terminal: its GOAWAY has been queued (unless the
    // failure was an allocation failure that left no room for it) and the I/O
    // owner must close the transport after flushing it. Graceful local/peer
    // GOAWAY is deliberately absent from this value.
    [[nodiscard]] std::optional<http2_error_code> connection_error() const noexcept {
        const auto* failure = local_connection_state_.fatal_failure();
        return failure != nullptr ? std::optional<http2_error_code>(failure->error()) : std::nullopt;
    }
    void begin_drain();
    void finish_drain();
    [[nodiscard]] bool draining() const noexcept {
        return local_connection_state_.graceful_drain() != nullptr;
    }
    [[nodiscard]] bool drain_final() const noexcept {
        const auto* drain = local_connection_state_.graceful_drain();
        return drain != nullptr && !drain->awaiting_round_trip();
    }
    [[nodiscard]] bool header_block_in_progress() const noexcept {
        return header_block_in_progress_ || header_block_.has_value();
    }
    // Queues the role-specific connection preface (client magic, SETTINGS and the
    // connection WINDOW_UPDATE) exactly once.
    void begin_connection();

    // --- client role -------------------------------------------------------------
    [[nodiscard]] std::size_t available_local_request_streams(std::size_t local_limit) const noexcept;
    [[nodiscard]] std::variant<std::uint32_t, http2_request_head_submit_error>
    submit_regular_request_head(std::string_view method,
        std::string_view scheme, std::optional<std::string_view> authority, std::string_view path,
        std::span<const http_header_view> headers, http2_request_content content,
        http_client_request_expectation expectation = http_client_request_expectation::none,
        http_request_field_source source = http_request_field_source::generated);
    [[nodiscard]] std::variant<std::uint32_t, http2_request_head_submit_error>
    submit_connect_request_head(
        std::string_view authority, std::span<const http_header_view> headers = {},
        http_request_field_source source = http_request_field_source::generated);
    [[nodiscard]] std::variant<std::uint32_t, http2_request_head_submit_error>
    submit_extended_connect_request_head(
        std::string_view protocol, std::string_view scheme, std::string_view authority,
        std::string_view path, std::span<const http_header_view> headers = {},
        http_request_field_source source = http_request_field_source::generated);

    // A DATA event borrows bytes from the accepted input and retains the matching
    // receive-window debt; acknowledging it hands the bytes to
    // nghttp2_session_consume(). False when the stream or its debt is gone.
    [[nodiscard]] bool acknowledge_received_data(std::uint32_t stream_id, std::uint32_t bytes);
    // Destruction path of an unacknowledged credit: never throws.
    void abandon_received_data(std::uint32_t stream_id, std::uint32_t bytes) noexcept;
    [[nodiscard]] http2_request_content_release_status release_request_content(
        std::uint32_t stream_id) noexcept;
    [[nodiscard]] std::size_t pending_data_output_bytes(std::uint32_t stream_id) const noexcept;
    [[nodiscard]] std::optional<http2_send_window_state> send_window_state(
        std::uint32_t stream_id) const noexcept;
    [[nodiscard]] bool stream_aborted(std::uint32_t stream_id) const noexcept;
    [[nodiscard]] bool received_peer_settings() const noexcept {
        return received_peer_settings_;
    }

    // A request/response built from a stream holds views into its decoded
    // storage, so the stream must outlive an in-flight handler. While pinned, a
    // peer reset only marks it aborted (emitting stream_closed) instead of
    // freeing it.
    void pin_stream(std::uint32_t stream_id) noexcept;
    void lease_submitted_request(
        const std::variant<std::uint32_t, http2_request_head_submit_error>& result) noexcept;
    // Ends an owner lease. The stream is unpinned once its delivered DATA credit
    // and queued events are gone; until then the release stays deferred.
    void release_stream_lease(std::uint32_t stream_id);
    // Response/tunnel EOF releases the client lease only after both halves close.
    void release_stream_lease_after_remote_end(std::uint32_t stream_id) noexcept;
    // Destruction path of an unreleased lease: resets a live stream, then
    // releases it. Never throws; a failure is retried later.
    void abandon_stream_lease(std::uint32_t stream_id) noexcept;
    void retry_deferred_stream_lease(std::uint32_t stream_id) noexcept;
    void retry_deferred_stream_leases() noexcept;
    // Sends whatever nghttp2 queued outside a flushing operation (WINDOW_UPDATE
    // after a credit, RST_STREAM after an abandoned lease). Failures have
    // already failed the connection and are observed by the next operation.
    void flush_output() noexcept;

private:
    friend struct http2_session_callbacks;

    struct session_deleter final {
        void operator()(nghttp2_session* session) const noexcept {
            nghttp2_session_del(session);
        }
    };

    // The field block nghttp2 is currently delivering through on_header.
    enum class header_block_kind : std::uint8_t {
        request,
        response,
        request_trailers,
        response_trailers,
        push_promise,
    };
    struct header_block_type final {
        header_block_type(header_block_kind kind, http2_stream_state& stream,
            std::pmr::memory_resource* resource) noexcept
            : kind_(kind),
              context_(stream),
              informational_fields_(resource) {}

        header_block_kind kind_;
        http2_header_decode_context context_;
        // Response heads: a 1xx keeps its fields separately from the final head.
        std::pmr::vector<http_header> informational_fields_;
        std::optional<http_received_status> status_;
        http_response_content_semantics content_semantics_{http_response_content_semantics::with_content};
        bool saw_regular_{false};
        // PUSH_PROMISE on a connected tunnel (RFC 9113 §8.5): decoded, then refused.
        bool reject_connected_tunnel_{false};
    };

    // One outbound DATA submission. It lives on submit_data()'s stack and
    // nghttp2 copies from it while the same call drains the session.
    struct outbound_data_type final {
        std::string_view bytes_;
        bool finished_{false};
    };

    // --- nghttp2 plumbing (callbacks run inside mem_recv2/mem_send2) ----------
    void create_session();
    // Drains every frame nghttp2 is ready to send into the output buffer and
    // surfaces any failure recorded by a callback.
    void flush_session();
    // Fails the connection for a negative nghttp2 return code and rethrows a
    // recorded callback exception or allocation failure.
    void fail_on_nghttp2_error(int error);
    // Submission results: fatal codes fail the connection and throw; any other
    // rejection is a caller contract violation (std::logic_error).
    void check_nghttp2(int result);
    // A local connection error: GOAWAY(error) through nghttp2, then terminal state.
    void fail_connection(http2_error_code error);
    void record_callback_failure() noexcept;

    int on_begin_frame(const nghttp2_frame_hd& header);
    int on_begin_headers(const nghttp2_frame& frame);
    int on_header(const nghttp2_frame& frame, std::string_view name, std::string_view value);
    int on_frame_recv(const nghttp2_frame& frame);
    int on_data_chunk_recv(std::uint8_t flags, std::uint32_t stream_id, std::string_view data);
    int on_stream_close(std::uint32_t stream_id, std::uint32_t error_code);
    int before_frame_send(const nghttp2_frame& frame);
    int on_frame_send(const nghttp2_frame& frame);
    int on_extension_chunk(const nghttp2_frame_hd& header, std::string_view data);

    int begin_request_headers(const nghttp2_frame& frame);
    int begin_response_headers(const nghttp2_frame& frame);
    int begin_push_promise(const nghttp2_frame& frame);
    // Validates one decoded field against the active block; false is malformed.
    [[nodiscard]] bool accept_header_field(std::string_view name, std::string_view value);
    void finish_header_block(const nghttp2_frame& frame);
    [[nodiscard]] bool finish_request_head(http2_stream_state& stream);
    [[nodiscard]] bool finish_response_head(http2_stream_state& stream, header_block_type& block);
    [[nodiscard]] bool finish_trailers(http2_stream_state& stream);
    void finish_push_promise(const nghttp2_frame& frame, header_block_type& block);
    // nghttp2 discards a PUSH_PROMISE whose associated stream closes before the
    // block ends; the promised stream it already reserved is cancelled here.
    void cancel_push_promise_on(std::uint32_t associated_stream_id);
    void finish_data_frame(http2_stream_state& stream);
    void process_rst_stream(const nghttp2_frame& frame);
    void process_goaway(const nghttp2_frame& frame);
    void process_settings(const nghttp2_frame& frame);
    void process_ping(const nghttp2_frame& frame);
    void process_priority_update(const nghttp2_frame& frame);
    void process_advertisement(const nghttp2_frame_hd& header);
    // A malformed field block: RST_STREAM(PROTOCOL_ERROR), plus the malformed
    // request-head budget for a server's initial head.
    void reject_header_block(std::uint32_t stream_id, bool malformed_request_head);

    // Emit message_head (and message_end when the peer already ended the stream).
    void emit_request_headers(http2_stream_state& stream);

    // --- stream lifecycle -------------------------------------------------------
    [[nodiscard]] http2_stream_state* create_stream(std::uint32_t stream_id);
    [[nodiscard]] std::optional<http2_request_head_submit_error> local_request_admission_error()
        const noexcept;
    // Queues the staged request fields as the next client stream's HEADERS.
    // A failure consumes neither a stream identifier nor a concurrency slot.
    template <typename prepare_type>
    [[nodiscard]] std::variant<std::uint32_t, http2_request_head_submit_error> submit_local_request_head(
        http2_end_stream end_stream, const prepare_type& prepare);
    void release_local_request_stream_if_closed(http2_stream_state& stream) noexcept;
    void release_local_request_stream(http2_stream_state& stream) noexcept;
    void unpin_stream(std::uint32_t stream_id);
    [[nodiscard]] bool release_received_data(std::uint32_t stream_id, std::uint32_t bytes);
    void abandon_stream_lease_checked(std::uint32_t stream_id);
    // Return consumed receive credit to nghttp2's WINDOW_UPDATE accounting.
    void consume_received(std::uint32_t stream_id, std::size_t bytes);
    // Preserve a removed stream's unacknowledged debt as connection credit.
    void flush_window_debt(http2_stream_state& stream);

    // Submits HEADERS for `stream` from the staged field list.
    void submit_header_frames(http2_stream_state& stream, http2_end_stream end_stream);
    [[nodiscard]] std::optional<http2_response_head_submit_error> response_head_admission_error(
        const http2_stream_state* stream, http_status_code status) const noexcept;
    [[nodiscard]] bool commit_final_response_head(http2_stream_state& stream, const http_response& response,
        const http_framed_response_head_plan& head_plan, http2_end_stream end_stream,
        std::optional<std::uint64_t> content_length);
    void commit_connect_response_head(http2_stream_state& stream, bool terminal_remote_half);
    // Sends `bytes` (already clamped to both send windows) as DATA within this
    // call; nghttp2 copies them while the session drains.
    void send_data(std::uint32_t stream_id, std::string_view bytes, http2_end_stream end_stream);
    // Commits the local END_STREAM, then sends it as empty DATA or with the
    // staged trailer fields.
    void end_local_stream(http2_stream_state& stream, bool empty_data);
    // nghttp2 DATA source: copies the submitting call's bytes. A null source is
    // the empty END_STREAM a rejected client CONNECT owes, which may wait for
    // flow-control credit inside nghttp2.
    static nghttp2_ssize read_outbound_data(nghttp2_session* session, std::int32_t stream_id,
        std::uint8_t* buffer, std::size_t length, std::uint32_t* data_flags, nghttp2_data_source* source,
        void* user_data) noexcept;
    // An empty END_STREAM DATA frame needs positive connection and stream send
    // windows (nghttp2 defers zero-window DATA).
    [[nodiscard]] bool can_send_empty_end_stream(std::uint32_t stream_id) const noexcept;
    [[nodiscard]] std::int32_t connection_send_window() const noexcept;
    [[nodiscard]] std::uint32_t peer_setting(nghttp2_settings_id id) const noexcept;

    // Appends an event into reserved capacity and counts it on every stream it names.
    void publish_event(http2_event event);

    // Peer-induced output is bounded without a clock; every limit ends in
    // GOAWAY(ENHANCE_YOUR_CALM). PING ACK, SETTINGS ACK and each RST_STREAM that
    // answers a peer frame count until output fully drains (Go x/net/http2
    // maxQueuedControlFrames); such resets also consume a lifetime budget, as h2
    // max_local_error_reset_streams.
    static constexpr std::uint32_t max_undrained_control_frames = 1000;
    static constexpr std::uint32_t max_peer_error_resets = 1024;
    // Rapid reset (CVE-2023-44487): peer resets may run this far ahead of the
    // responses this connection let complete.
    static constexpr std::uint32_t max_unprocessed_resets = 1000;
    // Pingora's lifetime bound on malformed server request heads.
    static constexpr std::uint32_t max_malformed_request_streams = 32;
    // CONTINUATION frames following one HEADERS/PUSH_PROMISE (CVE-2024-27316).
    static constexpr std::size_t max_continuation_frames = 1024;

    // Owner/local resets do not count against peer-induced budgets.
    void submit_owner_reset(std::uint32_t stream_id, http2_error_code error);
    // A stream error found in Ruvia's semantics: RST_STREAM and a local close event.
    void reset_stream_for_peer_error(std::uint32_t stream_id, http2_error_code error);

    enum class close_notification_type : std::uint8_t { emit_event,
        owner_already_knows };
    bool close_stream_impl(std::uint32_t stream_id, http2_stream_close_source source,
        http2_error_code error, close_notification_type notification);
    bool close_stream(std::uint32_t stream_id, http2_stream_close_source source, http2_error_code error);
    void finish_drain_round_trip();
    void begin_final_drain();
    void submit_goaway(std::uint32_t last_stream_id);

    std::pmr::memory_resource* resource_;
    http2_nghttp2_memory memory_;
    std::unique_ptr<nghttp2_session, session_deleter> session_;
    http2_output_buffer output_;
    http2_field_list fields_;
    http2_stream_table streams_;
    // RFC 9218 priority signals received for streams the peer has not opened yet.
    std::pmr::unordered_map<std::uint32_t, http_priority_fields> priorities_;
    std::pmr::vector<http2_event> events_;
    std::size_t event_offset_{0};
    http2_stream_lease_ledger leases_;
    std::pmr::vector<std::uint32_t> owner_resets_;
    std::pmr::string extension_payload_;
    std::optional<header_block_type> header_block_;
    // A PUSH_PROMISE's request decodes into scratch storage; the promised
    // stream's own receive state awaits the pushed response.
    std::optional<http2_stream_state> push_scratch_;
    std::uint32_t push_associated_stream_id_{0};
    std::exception_ptr callback_failure_;

    http2_local_connection_state local_connection_state_;
    http2_role role_{http2_role::server};
    bool enable_push_{false};
    bool receive_origin_advertisements_{false};
    bool receive_frame_metadata_{false};
    bool started_{false};
    bool received_peer_settings_{false};
    bool header_block_in_progress_{false};
    bool liveness_ping_outstanding_{false};
    std::uint32_t last_peer_stream_id_{0};
    std::uint32_t highest_peer_stream_id_{0};
    std::uint32_t active_local_request_streams_{0};
    std::optional<http2_goaway_event> peer_goaway_;
    std::size_t header_block_bytes_{0};

    std::uint32_t malformed_request_streams_{0};
    std::uint32_t peer_reset_streams_{0};
    std::uint32_t completed_responses_{0};
    std::uint32_t undrained_control_frames_{0};
    std::uint32_t peer_error_resets_{0};
};

}  // namespace ruvia::detail
