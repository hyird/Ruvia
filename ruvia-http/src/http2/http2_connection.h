#pragma once

#include "ruvia/http/http2_types.h"
#include "ruvia/http/http_header.h"

// HTTP/2 sans-I/O connection core.
//
// A pure protocol state machine: it never touches a socket, a coroutine, a timer,
// or asio. You feed it inbound bytes and it advances the protocol and emits events
// (request ready, body chunk, stream closed, ...); you submit responses and it
// produces outbound bytes for you to write. The I/O loop, concurrency model and
// timeouts live entirely in the caller; any runtime can drive it, nghttp2-style.
//
// Design mirrors nghttp2's mem_recv / mem_send: feed() ~ nghttp2_session_mem_recv,
// pending_output()/consume_output() ~ nghttp2_session_mem_send. Flow-control back
// pressure has explicit ownership: submit_data either accepts the full input (possibly
// copying a deferred suffix) or accepts none until the prior queued input drains.
// Final response Content-Length uses the same ownership boundary: accepted bytes are
// counted once for the whole input, committed bytes advance only when DATA is framed,
// and a mismatch is rejected before output/window/state mutation.
//
// All protocol primitives it builds on (frame codec, HPACK, stream state, flow
// control, input buffer, settings) are already pure and reused as-is. Generic frame
// progression, the connection-global header-block lifecycle, and local submission
// live in separate implementation units; http2_output_buffer is the sole owner of
// outbound storage and its consumed cursor.

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
#include "ruvia/http/detail/server/http_response_write_plan.h"
#include "ruvia/http/detail/util/borrowed_view.h"
#include "ruvia/http/http2_request_content.h"
#include "ruvia/http/http2_request_head_submit_result.h"
#include "ruvia/http/http2_response_head_submit_result.h"
#include "ruvia/http/http2_types.h"
#include "ruvia/http/http_client.h"
#include "ruvia/http/http_interim_response.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_response_stream.h"
#include "ruvia/http/websocket_handshake.h"

#include "http2/http2_closed_streams.h"
#include "http2/http2_event.h"
#include "http2/http2_frame_types.h"
#include "http2/http2_header_continuation.h"
#include "http2/http2_header_decode.h"
#include "http2/http2_hpack.h"
#include "http2/http2_local_connection_state.h"
#include "http2/http2_local_settings.h"
#include "http2/http2_output_buffer.h"
#include "http2/http2_peer_settings.h"
#include "http2/http2_ready_queue.h"
#include "http2/http2_receive_window_credit.h"
#include "http2/http2_role.h"
#include "http2/http2_stream_state.h"
#include "http2/http2_stream_table.h"
#include "websocket/websocket_server_negotiation.h"

namespace ruvia::detail {

using ruvia::http2_data_submit_status;
using ruvia::http2_end_stream;
using ruvia::http2_ends_stream;
using ruvia::http2_feed_result;
using ruvia::http2_output_consume_status;
using ruvia::http2_request_content_release_status;
using ruvia::http2_request_head_submit_error;
using ruvia::http2_submit_status;

enum class http2_websocket_handshake_submit_error : std::uint8_t {
    closed,
    invalid_state,
};

class http2_websocket_handshake_submit_result;

class http2_websocket_handshake_submit_failure final {
public:
    [[nodiscard]] constexpr http2_websocket_handshake_submit_error error() const noexcept {
        return error_;
    }

private:
    friend class http2_websocket_handshake_submit_result;

    explicit constexpr http2_websocket_handshake_submit_failure(
        http2_websocket_handshake_submit_error error) noexcept
        : error_(error) {}

    http2_websocket_handshake_submit_error error_;
};

// The successful alternative directly owns the exact negotiation encoded in
// the 200 response. The runtime must configure ws_connection from that committed
// value, never from a separately recomputed compression/subprotocol tuple.
class http2_websocket_handshake_submit_result final {
public:
    http2_websocket_handshake_submit_result(const http2_websocket_handshake_submit_result&) = delete;
    http2_websocket_handshake_submit_result& operator=(
        const http2_websocket_handshake_submit_result&) = delete;
    http2_websocket_handshake_submit_result(http2_websocket_handshake_submit_result&&) noexcept = default;
    http2_websocket_handshake_submit_result& operator=(http2_websocket_handshake_submit_result&&) = delete;

    [[nodiscard]] const websocket_server_negotiation* submitted() const& noexcept {
        return (value_.index() == 0) ? &std::get<0>(value_) : nullptr;
    }
    [[nodiscard]] const websocket_server_negotiation* submitted() const&& = delete;

    [[nodiscard]] const http2_websocket_handshake_submit_failure* failure() const& noexcept {
        return (value_.index() == 0) ? nullptr : &std::get<1>(value_);
    }
    [[nodiscard]] const http2_websocket_handshake_submit_failure* failure() const&& = delete;

private:
    friend class http2_connection;

    using value_type = std::variant<websocket_server_negotiation, http2_websocket_handshake_submit_failure>;

    explicit http2_websocket_handshake_submit_result(websocket_server_negotiation&& negotiation) noexcept
        : value_(std::move(negotiation)) {}

    explicit http2_websocket_handshake_submit_result(
        http2_websocket_handshake_submit_failure failure) noexcept
        : value_(failure) {}

    [[nodiscard]] static http2_websocket_handshake_submit_result make_submitted(
        websocket_server_negotiation&& negotiation) noexcept {
        return http2_websocket_handshake_submit_result(std::move(negotiation));
    }

    [[nodiscard]] static http2_websocket_handshake_submit_result make_failure(
        http2_websocket_handshake_submit_error error) noexcept {
        return http2_websocket_handshake_submit_result(http2_websocket_handshake_submit_failure(error));
    }

    value_type value_;
};

enum class http2_finish_submit_status : std::uint8_t {
    accepted,
    queued,
    // The stream disappeared or was reset while its owner was finishing it.
    closed,
    // The stream is not in an open response body/trailers phase, or a
    // trailers-only response has no submitted terminal section.
    invalid_state,
    // The response remains body-open so the caller can submit the missing bytes.
    content_length_incomplete
};

// A final response HEADERS transaction either commits one body/stream plan or
// rejects the submission without exposing a plan that was never committed. The
// refusal reason is a plain enum, matching the request-head submission contract;
// protocol layers never manufacture exception objects.

[[nodiscard]] inline std::string_view http2_response_head_submit_error_message(
    http2_response_head_submit_error error) noexcept {
    switch (error) {
        case http2_response_head_submit_error::peer_stream_limit_reached:
            return "peer concurrent stream limit reached";
        case http2_response_head_submit_error::closed:
            return "HTTP/2 response stream is closed";
        case http2_response_head_submit_error::invalid_state:
            return "invalid HTTP/2 response head submission state";
        case http2_response_head_submit_error::response_plan_mismatch:
            return "HTTP/2 response head does not match its write plan";
        case http2_response_head_submit_error::invalid_message:
            return "invalid HTTP/2 response head message";
    }
    return "unknown HTTP/2 response head submission failure";
}

// A response body the send window could not fully drain: the core keeps the unsent
// remainder and flushes it as WINDOW_UPDATE/SETTINGS reopen the window (nghttp2-style
// deferred data). At most one queued submission exists per stream.
struct http2_pending_send final {
    std::uint32_t stream_id_{0};
    std::pmr::string bytes_;
    std::size_t offset_{0};
    http2_end_stream end_stream_{http2_end_stream::keep_open};
    // A terminal trailer HEADERS block queued atomically by finish_response behind
    // flow-control-deferred DATA. It goes out AFTER that DATA drains (RFC 9113
    // §8.1), carrying END_STREAM in place of the body.
    std::pmr::string trailer_block_;
};

class http2_connection final {
    // One role-aware receive phase owns connection startup. Keeping this as one enum
    // prevents combinations such as "started but not awaiting magic or SETTINGS".
    enum class preface_phase_type : std::uint8_t {
        not_started,
        awaiting_client_magic,
        awaiting_peer_settings,
        ready
    };

public:
    explicit http2_connection(
        std::pmr::memory_resource* resource, http2_role role = http2_role::server, bool enable_push = false, bool receive_origin_advertisements = false);

    [[nodiscard]] http2_role role() const noexcept {
        return role_;
    }

    // --- inbound ---------------------------------------------------------------
    // Feed raw bytes read from the peer; advances the protocol. Before
    // begin_connection(), connection_not_started retains the exact input for retry.
    // Calling while events remain returns events_pending with the same retained-input
    // guarantee; drain next_event() first. accepted and need_input both accept the
    // whole input, with the latter buffering a partial preface/frame. protocol_failure
    // is terminal, so that input must be dropped. Every accepted call can emit events,
    // which must be drained before the next input is offered.
    [[nodiscard]] http2_feed_result feed(std::string_view in);
    template <http_temporary_owning_char_string input_type>
    http2_feed_result feed(input_type&&) = delete;
    // Pull the next protocol event. nullopt means the queue is drained; every
    // materialized event contains exactly one typed payload.
    [[nodiscard]] std::optional<http2_event> next_event();
    // Facades that must perform fallible event materialization use peek/commit so
    // an exception never consumes the only protocol notification for a stream.
    [[nodiscard]] http2_event* peek_event() & noexcept;
    [[nodiscard]] http2_event* peek_event() && = delete;
    [[nodiscard]] bool has_pending_events(std::uint32_t stream_id) const noexcept;
    void consume_event() noexcept;

    // Access an assembled request head / stream for the owner to build an http_request.
    [[nodiscard]] http2_stream_state* stream(std::uint32_t stream_id) & noexcept;
    [[nodiscard]] http2_stream_state* stream(std::uint32_t) && = delete;
    // Read the peer's receive half-state without exposing stream storage.
    [[nodiscard]] http2_stream_receive_status stream_receive_status(
        std::uint32_t stream_id) const noexcept;
    // Borrowed server request metadata snapshot; never exposes stream storage.
    [[nodiscard]] std::optional<http2_server_request_view> server_request_view(
        std::uint32_t stream_id) const noexcept;

    // --- outbound --------------------------------------------------------------
    // Bytes the core wants written to the peer (frame headers + payloads, batched).
    [[nodiscard]] std::string_view pending_output() const& noexcept;
    [[nodiscard]] std::string_view pending_output() const&& = delete;
    // Acknowledges at most the current pending size. An out-of-range count is
    // rejected without clearing bytes or advancing the cursor.
    http2_output_consume_status consume_output(std::size_t bytes) noexcept;
    // Move ALL pending outbound bytes into `into` (allocator-matching swap when nothing
    // was partially consumed, so the common path is copy-free) and reset the buffer.
    // REQUIRED for any writer that awaits mid-write: a pending_output() view dangles if
    // a concurrent submit reallocates the buffer during the write.
    void take_output(std::pmr::string& into);
    [[nodiscard]] http2_output_batch_result take_output_batch(std::size_t max_bytes,
        std::pmr::string& into, http2_data_output_observer_type observer, void* observer_context);
    [[nodiscard]] bool wants_write() const noexcept {
        return output_.wants_write();
    }

    // Submit a final response for `stream_id`. The caller must pass the write plan
    // prepared after its last body/header transformation; method provenance,
    // status, and representation length are checked against the live stream and
    // response. A stale/mismatched plan is rejected distinctly before HPACK or
    // stream mutation. Informational status codes and HTTP/2-unrepresentable
    // control semantics (notably 426, whose mandatory Upgrade field is forbidden
    // here) are likewise rejected transactionally. An exclusive
    // http2_response_head_plan owns canonical, explicit, absent, or forbidden
    // Content-Length metadata before the encoder and local DATA state advance.
    [[nodiscard]] http2_response_head_submit_result submit_response_head(std::uint32_t stream_id,
        const http_response& response, http_buffered_response_write_plan write_plan);
    // Submit a STREAMING response head: no Content-Length is generated automatically;
    // an explicit value is strictly parsed once and the same plan binds both HPACK
    // metadata and all later DATA. With no explicit value the body is unbounded.
    // The stream stays open for subsequent submit_data
    // chunks unless the method/status suppresses a body. A declared trailer section
    // keeps an HTTP/2 content-forbidden response open in a trailers-only phase; without
    // one, END_STREAM is carried by the initial HEADERS. The owner then streams DATA
    // (when allowed) and terminates through finish_response(stream_id, trailers).
    [[nodiscard]] http2_streaming_response_head_submit_result submit_streaming_response_head(
        std::uint32_t stream_id, http_response head, http_response_stream_kind kind,
        http_response_trailer_intent trailer_intent);
    [[nodiscard]] http2_data_submit_status submit_data(
        std::uint32_t stream_id, std::string_view chunk, http2_end_stream end_stream);
    // Submit a typed interim 1xx head. http_interim_response_head excludes 101 and
    // cannot carry content; the initial-head phase remains open for the required
    // final response. Invalid HTTP/2 fields reject transactionally.
    [[nodiscard]] http2_submit_status submit_interim_response_head(
        std::uint32_t stream_id, const http_interim_response_head& response);
    // Queue the RFC 8441 successful response (:status 200, Date and the exact
    // negotiated fields, without END_STREAM) and open the stream as a tunnel.
    // Only the submitted alternative exposes the negotiation committed on wire.
    // Ownership moves into that alternative only after validation succeeds; a
    // rejected submission leaves the caller's negotiation unchanged.
    [[nodiscard]] http2_websocket_handshake_submit_result submit_websocket_handshake(
        std::uint32_t stream_id, websocket_server_negotiation&& negotiation);
    [[nodiscard]] http2_websocket_handshake_submit_result submit_websocket_handshake(
        std::uint32_t stream_id, const websocket_handshake_validation_result& validation,
        websocket_server_negotiation&& negotiation);
    [[nodiscard]] http2_websocket_handshake_submit_result submit_websocket_handshake(
        std::uint32_t stream_id, const http_request& request,
        const websocket_handshake_validation_result& validation);
    // Accept a pending standard or extended CONNECT with a successful final response.
    // The head must be bodyless and contain neither Content-Length nor
    // Transfer-Encoding. DATA becomes opaque tunnel bytes only after this succeeds.
    [[nodiscard]] http2_submit_status submit_connect_response_head(
        std::uint32_t stream_id, const http_response& response);
    // Finish a response exactly once with its complete terminal trailer section
    // (possibly empty). Its typed value proves shared validation already completed;
    // HPACK encoding, DATA/trailer ordering, and END_STREAM are one transaction.
    // An incomplete declared Content-Length is rejected without changing the
    // body-open phase. A flow-control-blocked body keeps the
    // terminal marker queued behind it once the full length is core-owned.
    [[nodiscard]] std::variant<std::uint32_t, http2_push_submit_error> submit_push_promise(
        std::uint32_t associated_stream_id, http_push_request_view request);
    [[nodiscard]] http2_finish_request_status finish_request(std::uint32_t stream_id,
        std::span<const http_header_view> trailers = {});
    [[nodiscard]] http2_finish_submit_status finish_response(
        std::uint32_t stream_id, const http_response_trailer_section& trailers);
    [[nodiscard]] http2_submit_status submit_origin_advertisement(std::span<const std::string_view> origins);
    [[nodiscard]] http2_submit_status submit_alternative_service_advertisement(std::uint32_t stream_id,
        std::string_view origin, std::string_view field_value);
    [[nodiscard]] http2_submit_status submit_priority_update(std::uint32_t stream_id, http_priority_fields fields);

    [[nodiscard]] http2_submit_status submit_reset(std::uint32_t stream_id, http2_error_code error);

    // Returns streams whose core-owned DATA remainder just fully drained after a
    // WINDOW_UPDATE/SETTINGS change. Their owner may now submit the next source chunk.
    [[nodiscard]] std::span<const std::uint32_t> take_drained_data_streams() & noexcept;
    [[nodiscard]] std::span<const std::uint32_t> take_drained_data_streams() && = delete;

    // --- lifecycle / timeout ---------------------------------------------------
    // A local connection error is terminal: its GOAWAY has been queued and the I/O
    // owner must close the transport after flushing it. Graceful local/peer GOAWAY is
    // deliberately absent from this value; those connections keep established streams
    // alive while draining.
    [[nodiscard]] std::optional<http2_error_code> connection_error() const noexcept {
        const auto* failure = local_connection_state_.fatal_failure();
        return failure != nullptr ? std::optional<http2_error_code>(failure->error()) : std::nullopt;
    }

    // Graceful local drain: advertise GOAWAY(NO_ERROR) at the current last peer stream
    // id and keep serving streams already accepted; HEADERS for a stream above the
    // advertised id are refused (RST_STREAM(REFUSED_STREAM)). Idempotent. Receiving a
    // valid peer GOAWAY enters this state through the same path so shutdown is bilateral.
    void begin_drain();
    [[nodiscard]] bool draining() const noexcept {
        return local_connection_state_.graceful_drain() != nullptr;
    }

    // True while a HEADERS block is still being assembled (awaiting CONTINUATION); the
    // I/O layer maps this to its tight header-read inactivity timeout.
    [[nodiscard]] bool header_block_in_progress() const noexcept {
        return header_continuation_.active();
    }

    // Begin the role-specific HTTP/2 connection preface exactly once. Client role
    // queues the 24-byte magic plus SETTINGS; server role queues SETTINGS and makes
    // feed() require the peer magic before its first frame. In both roles the first
    // peer frame must then be a non-ACK SETTINGS frame. Both append the matching
    // connection WINDOW_UPDATE. Repeated calls are idempotent.
    void begin_connection();

    // --- client role -------------------------------------------------------------
    // Validate, open the next odd stream, and queue a regular request HEADERS block as
    // one transaction. Failure consumes neither a stream ID nor a peer concurrency
    // slot. `content` is the sole Content-Length/END_STREAM contract; later content
    // flows via submit_data(result.submitted()->stream_id(), ...).
    // CONNECT has different pseudo-header and tunnel semantics and is deliberately
    // rejected here; it uses the dedicated CONNECT submission path. `scheme`
    // accepts the complete RFC 3986 grammar, including non-HTTP schemes. `path`
    // is origin-form, except that only OPTIONS can use asterisk-form. `authority`
    // is absent exactly when the target URI has no authority information. HTTP(S)
    // origin-form requires Host-compatible authority without userinfo; other
    // schemes use the complete RFC 3986 authority grammar and may carry an empty
    // path. Asterisk-form OPTIONS itself has no authority component, but a direct
    // HTTP/2 sender may still carry request authority in :authority.
    [[nodiscard]] http2_request_head_submit_result submit_regular_request_head(std::string_view method,
        std::string_view scheme, std::optional<std::string_view> authority, std::string_view path,
        std::span<const http_header_view> headers, http2_request_content content,
        http_client_request_expectation expectation = http_client_request_expectation::none);
    // The submitted head is queued for later HPACK encoding, so every view must
    // outlive the call; a temporary owning string would be destroyed while the
    // borrowed head is still pending. Owning temporaries are rejected at
    // compile time, one deleted overload per view parameter.
    template <detail::http_temporary_owning_char_string method_type>
    http2_request_head_submit_result submit_regular_request_head(method_type&&, std::string_view,
        std::optional<std::string_view>, std::string_view, std::span<const http_header_view>,
        http2_request_content,
        http_client_request_expectation = http_client_request_expectation::none) = delete;
    template <detail::http_temporary_owning_char_string scheme_type>
    http2_request_head_submit_result submit_regular_request_head(std::string_view, scheme_type&&,
        std::optional<std::string_view>, std::string_view, std::span<const http_header_view>,
        http2_request_content,
        http_client_request_expectation = http_client_request_expectation::none) = delete;
    template <typename authority_type>
        requires detail::http_temporary_owning_char_string<authority_type>
    http2_request_head_submit_result submit_regular_request_head(std::string_view, std::string_view,
        std::optional<authority_type>&&, std::string_view, std::span<const http_header_view>,
        http2_request_content,
        http_client_request_expectation = http_client_request_expectation::none) = delete;
    template <detail::http_temporary_owning_char_string path_type>
    http2_request_head_submit_result submit_regular_request_head(std::string_view, std::string_view,
        std::optional<std::string_view>, path_type&&, std::span<const http_header_view>,
        http2_request_content,
        http_client_request_expectation = http_client_request_expectation::none) = delete;
    // Standard CONNECT uses only :method and an authority-form :authority. Its
    // initial HEADERS never ends the stream; DATA is gated until a final 2xx response.
    [[nodiscard]] http2_request_head_submit_result submit_connect_request_head(
        std::string_view authority, std::span<const http_header_view> headers = {});
    template <detail::http_temporary_owning_char_string authority_type>
    http2_request_head_submit_result submit_connect_request_head(
        authority_type&&, std::span<const http_header_view> = {}) = delete;
    // RFC 8441 Extended CONNECT. The peer must first advertise
    // SETTINGS_ENABLE_CONNECT_PROTOCOL=1. :protocol is a protocol-name token and the
    // ordinary target pseudo-headers are generated by the core. Generic protocols
    // accept any RFC 3986 scheme and allow its path to be empty; otherwise the path
    // is origin-form. websocket requires an HTTP(S) scheme. websocket protocol names
    // are matched case-insensitively and encoded as `websocket`.
    [[nodiscard]] http2_request_head_submit_result submit_extended_connect_request_head(
        std::string_view protocol, std::string_view scheme, std::string_view authority,
        std::string_view path, std::span<const http_header_view> headers = {});
    template <detail::http_temporary_owning_char_string protocol_type>
    http2_request_head_submit_result submit_extended_connect_request_head(protocol_type&&, std::string_view,
        std::string_view, std::string_view, std::span<const http_header_view> = {}) = delete;
    template <detail::http_temporary_owning_char_string scheme_type>
    http2_request_head_submit_result submit_extended_connect_request_head(std::string_view, scheme_type&&,
        std::string_view, std::string_view, std::span<const http_header_view> = {}) = delete;
    template <detail::http_temporary_owning_char_string authority_type>
    http2_request_head_submit_result submit_extended_connect_request_head(std::string_view,
        std::string_view, authority_type&&, std::string_view,
        std::span<const http_header_view> = {}) = delete;
    template <detail::http_temporary_owning_char_string path_type>
    http2_request_head_submit_result submit_extended_connect_request_head(std::string_view,
        std::string_view, std::string_view, path_type&&, std::span<const http_header_view> = {}) = delete;
    // A DATA event borrows bytes from the accepted input and retains the matching
    // receive-window debt. Once the owner has copied/consumed every currently
    // delivered DATA event for this stream, transfer the debt into batched
    // connection/stream credit. WINDOW_UPDATE is emitted only at the shared
    // half-window threshold. Safe when the stream is gone.
    [[nodiscard]] bool release_received_data(std::uint32_t stream_id, std::uint32_t bytes);
    void release_all_received_data(std::uint32_t stream_id);
    // Runtime timeout/manual release for an Expect-gated request body. A later
    // 100 response remains observable but emits no duplicate Continue signal.
    [[nodiscard]] http2_request_content_release_status release_request_content(
        std::uint32_t stream_id) noexcept;
    // True while submit_data left a window-blocked remainder queued for this stream
    // (the owner waits for the drain report before pulling its next body chunk).
    [[nodiscard]] bool has_queued_data(std::uint32_t stream_id) const noexcept;
    [[nodiscard]] http2_data_queue_state data_queue_state(std::uint32_t stream_id) const noexcept;
    [[nodiscard]] std::size_t pending_data_output_bytes(std::uint32_t stream_id) const noexcept;
    [[nodiscard]] std::optional<http2_send_window_state> send_window_state(
        std::uint32_t stream_id) const noexcept;
    [[nodiscard]] bool stream_aborted(std::uint32_t stream_id) const noexcept;
    // RFC 8441 capability only. A dedicated CONNECT/tunnel submission API must still
    // own pseudo-header shape and tunnel lifecycle; regular requests never infer it.
    [[nodiscard]] bool peer_extended_connect_enabled() const noexcept {
        return role_ == http2_role::client && peer_settings_.enable_connect_protocol();
    }
    // Peer handshake / lifecycle observability for client drivers.
    [[nodiscard]] bool received_peer_settings() const noexcept {
        return preface_phase_ == preface_phase_type::ready;
    }
    [[nodiscard]] std::optional<http2_peer_goaway> peer_goaway() const noexcept {
        return peer_goaway_;
    }

    // Concurrent dispatch support: a request/response built from a stream holds VIEWS
    // into that stream's decoded storage, so the stream must outlive an in-flight
    // (possibly-suspended) handler. Pin the stream before spawning its handler; while
    // pinned, a peer RST_STREAM/close only marks it reset (keeping the storage, and
    // emitting stream_closed so the owner can drop the response) instead of freeing it.
    // Unpin when the handler finishes; the stream is then removed. No-op / safe if the
    // stream is already gone.
    void pin_stream(std::uint32_t stream_id);
    void unpin_stream(std::uint32_t stream_id);

private:
    void append_goaway(http2_error_code error, std::string_view debug = {});

    // A WINDOW_UPDATE/SETTINGS change drains core-owned DATA remainders into the
    // outbound buffer. A stream whose remainder fully drains is reported through
    // drained_data_streams_ so the owner can pull its next source chunk.
    void mark_send_window_opened();

    // Emit a response header block as HEADERS + CONTINUATION frames (atomic sequence,
    // RFC 9113 §6.10) into the outbound buffer, ending the stream when end_stream is set.
    void append_response_header_frames(
        http2_stream_state& stream, std::string_view header_block, http2_end_stream end_stream);
    void commit_connect_response_head(http2_stream_state& stream, bool terminal_remote_half);
    // Emit DATA frames for data.substr(offset) while the send window allows, returning
    // the new offset (== data.size() when fully sent). Consumes send-window credit.
    [[nodiscard]] std::size_t send_data_up_to_window(http2_stream_state& stream, std::string_view data,
        std::size_t offset, http2_end_stream end_stream);

    // Consume complete frames from `buffer` starting at `offset` (advanced past each
    // consumed frame; a trailing partial frame is left for the caller). Returns false on
    // a fatal protocol error (GOAWAY queued). Shared by feed()'s fast (parse over the
    // caller's bytes) and slow (parse the buffered input_) paths.
    [[nodiscard]] bool consume_frames(std::string_view buffer, std::size_t& offset);
    // Synchronous per-frame dispatch (ported 1:1 from process_frame/*; returns false
    // on a fatal protocol error, having appended GOAWAY and transitioned the
    // local connection state to fatal failure).
    [[nodiscard]] bool process_frame(const http2_frame_header& header, std::string_view payload);
    [[nodiscard]] bool process_settings(const http2_frame_header& header, std::string_view payload);
    [[nodiscard]] bool process_ping(const http2_frame_header& header, std::string_view payload);
    [[nodiscard]] bool process_window_update(
        const http2_frame_header& header, std::string_view payload);
    [[nodiscard]] bool process_rst_stream(const http2_frame_header& header, std::string_view payload);
    [[nodiscard]] bool process_push_promise(const http2_frame_header& header, std::string_view payload);
    [[nodiscard]] bool finish_push_promise();
    [[nodiscard]] bool process_push_continuation(const http2_frame_header& header, std::string_view payload);
    [[nodiscard]] bool process_advertisement(const http2_frame_header& header, std::string_view payload);
    [[nodiscard]] bool process_priority_update(const http2_frame_header& header, std::string_view payload);
    [[nodiscard]] bool process_priority(const http2_frame_header& header, std::string_view payload);
    [[nodiscard]] bool process_goaway(const http2_frame_header& header, std::string_view payload);
    [[nodiscard]] bool process_headers(const http2_frame_header& header, std::string_view payload);
    [[nodiscard]] bool process_trailer_headers(
        http2_stream_state& stream, const http2_frame_header& header, std::string_view fragment);
    [[nodiscard]] bool process_continuation(
        const http2_frame_header& header, std::string_view payload);
    [[nodiscard]] bool process_data(const http2_frame_header& header, std::string_view payload);
    // Release a successfully debited DATA payload that protocol semantics discard.
    // Only connection credit survives because the stream is closed/being abandoned.
    void release_dropped_data_connection_window(std::int32_t flow_bytes);
    void queue_consumed_data_credit(http2_stream_state* stream, std::uint32_t bytes);
    [[nodiscard]] bool apply_settings_payload(std::string_view payload);

    // HPACK header-block decode (all pure; ported 1:1 from the coroutine session but
    // WITHOUT resolve_stream_route -- route resolution is application policy the owner runs
    // after pulling message_head). Return the classification; the caller reacts.
    [[nodiscard]] header_decode_status decode_header_block(http2_stream_state& stream,
        http2_stream_header_decode_transaction& stream_transaction,
        hpack_decoder::decode_transaction_type& hpack_transaction);
    // Client role: decode a RESPONSE header block (:status + regular headers into the
    // stream's header table). A 1xx interim head is validated then discarded WITHOUT
    // leaving the remote head-pending alternative active, so the next HEADERS block
    // decodes as the real head; callers emit events only after that alternative changes.
    [[nodiscard]] header_decode_status decode_response_header_block(http2_stream_state& stream,
        http2_stream_header_decode_transaction& stream_transaction,
        hpack_decoder::decode_transaction_type& hpack_transaction);
    // Role-aware initial-head decode dispatch (request vs response semantics).
    [[nodiscard]] header_decode_status decode_initial_header_block(http2_stream_state& stream,
        http2_stream_header_decode_transaction& stream_transaction,
        hpack_decoder::decode_transaction_type& hpack_transaction);
    // Role-aware idle-stream test (server: above the highest peer id; client: any even
    // id or an odd id we have not opened yet).
    [[nodiscard]] bool is_idle_stream_id(std::uint32_t stream_id) const noexcept;
    [[nodiscard]] header_decode_status decode_refused_header_block(
        http2_stream_state& stream, hpack_decoder::decode_transaction_type& hpack_transaction);
    [[nodiscard]] header_decode_status decode_discarded_header_block(
        http2_stream_state& stream, hpack_decoder::decode_transaction_type& hpack_transaction);
    [[nodiscard]] header_decode_status finish_trailer_block(http2_stream_state& stream,
        http2_stream_header_decode_transaction& stream_transaction,
        hpack_decoder::decode_transaction_type& hpack_transaction);
    template <http2_header_block_kind kind>
    [[nodiscard]] bool complete_decoded_header_block(http2_stream_state& stream);
    // On a decode failure: compression error is fatal (GOAWAY, returns false); anything
    // else RST_STREAMs the stream and survives (returns true).
    [[nodiscard]] bool handle_header_decode_failure(http2_stream_state& stream,
        header_decode_status status, hpack_decoder::decode_transaction_type* hpack_transaction);
    // Emit message_head (and message_end when the peer already ended the stream)
    // for the sans-I/O owner to dispatch.
    void emit_request_headers(http2_stream_state& stream);

    // A stream error or a locally-sent RST does not permit skipping a HEADERS block:
    // HPACK is connection-scoped, so every fragment must be accumulated and decoded.
    // The detached scratch prevents discarded fields from mutating a live/reset stream;
    // `discarded_header_action` is applied only after END_HEADERS.
    enum class discarded_header_action_type : std::uint8_t {
        ignore,
        reset_protocol_error,
        reset_stream_closed,
        refuse_stream
    };
    [[nodiscard]] bool start_discarded_header_block(
        const http2_frame_header& header, std::string_view fragment, discarded_header_action_type action);
    [[nodiscard]] bool finish_discarded_header_block();
    void detach_active_header_block(http2_stream_state& stream);

    [[nodiscard]] http2_stream_state* find_stream(std::uint32_t stream_id) noexcept;
    [[nodiscard]] http2_stream_state* create_stream(std::uint32_t stream_id);
    [[nodiscard]] std::optional<http2_request_head_submit_error> local_request_admission_error()
        const noexcept;
    [[nodiscard]] http2_stream_state* admit_local_request_stream();
    [[nodiscard]] http2_request_head_submit_result publish_local_request_head(
        http2_stream_state& stream) noexcept;
    template <typename prepare_type>
    [[nodiscard]] http2_request_head_submit_result submit_local_request_head(prepare_type&& prepare) {
        auto* stream = admit_local_request_stream();
        if (stream == nullptr) {
            return http2_request_head_submit_result::make_failure(
                http2_request_head_submit_error::local_stream_capacity_reached);
        }
        const auto stream_id = stream->id();
        try {
            std::forward<prepare_type>(prepare)(*stream);
        } catch (...) {
            (void)streams_.remove(stream_id);
            next_local_stream_id_ -= 2;
            throw;
        }
        return publish_local_request_head(*stream);
    }
    void release_local_request_stream_if_closed(http2_stream_state& stream) noexcept;
    void retire_completed_local_push(std::uint32_t stream_id);
    void release_local_request_stream(http2_stream_state& stream) noexcept;
    [[nodiscard]] bool is_pinned(std::uint32_t stream_id) const noexcept;

    void reserve_event_slots(std::size_t count);

    // Close a stream: drop it from the ready queue, mark closed, emit stream_closed
    // (so the owner cancels any handler), remove it, and remember it as closed.
    enum class close_notification_type : std::uint8_t { emit_event,
        owner_already_knows };
    bool close_stream_impl(std::uint32_t stream_id, http2_stream_close_source source,
        http2_error_code error, close_notification_type notification);
    bool close_stream(std::uint32_t stream_id, http2_stream_close_source source, http2_error_code error);
    bool close_stream_by_owner(std::uint32_t stream_id);
    [[nodiscard]] bool was_closed_by_peer_reset(
        std::uint32_t stream_id, const http2_stream_state* retained_stream) const noexcept;
    void discard_deferred_stream_state(std::uint32_t stream_id);
    // Preserve a removed stream's banked debt as batched connection credit.
    void flush_window_debt(http2_stream_state& stream);
    void reserve_stream_close_effects(http2_stream_state& stream);

    std::pmr::memory_resource* resource_;

    // Inbound byte buffer (reused across feeds; input_offset_ = consumed cursor).
    // A slow-path feed owns the supplied span here before dispatching it. If a later
    // frame throws, retry_input_ keeps that owned batch and its committed cursor so a
    // retry of the same caller span resumes at the first uncommitted frame instead of
    // appending and replaying the already committed prefix.
    std::pmr::string input_;
    std::size_t input_offset_{0};
    bool retry_input_{false};

    // Outbound serialization and consumed-prefix ownership are isolated from
    // connection state transitions.
    http2_output_buffer output_;

    // pure protocol state (all reused as-is)
    http2_stream_table streams_;
    std::pmr::unordered_map<std::uint32_t, http_priority_fields> priorities_;
    http2_closed_stream_history closed_streams_;
    http2_ready_queue ready_queue_;
    hpack_decoder decoder_;
    http2_header_continuation header_continuation_;
    http2_peer_settings peer_settings_;
    // The static-only encoder starts with HPACK's implicit 4096-byte maximum even
    // though it never inserts entries. A peer reduction below this value must still
    // be acknowledged on the wire at the beginning of the next field block.
    std::uint32_t encoder_dynamic_table_size_{http2_local_settings::header_table_size};
    bool encoder_table_size_update_pending_{false};
    std::optional<http2_stream_state> push_header_stream_;
    std::uint32_t push_associated_stream_id_{0};
    bool enable_push_{false};
    bool receive_origin_advertisements_{false};
    std::uint32_t last_peer_push_stream_id_{0};
    std::uint32_t next_push_stream_id_{2};
    std::optional<http2_stream_state> discarded_header_stream_;
    discarded_header_action_type discarded_header_action_{discarded_header_action_type::ignore};

    // event queue drained by next_event()
    std::pmr::vector<http2_event> events_;
    std::size_t event_offset_{0};

    // flow-control-deferred response bodies + streams that just fully drained
    std::pmr::vector<http2_pending_send> pending_sends_;
    std::pmr::vector<std::uint32_t> drained_data_streams_;
    std::pmr::vector<std::uint32_t> taken_drained_data_streams_;  // double buffer for returned spans

    // streams with an in-flight handler; close_stream keeps these alive (see pin_stream)
    std::pmr::vector<std::uint32_t> pinned_streams_;

    std::uint32_t local_max_frame_size_{http2_local_settings::max_frame_size};
    std::uint32_t last_stream_id_{0};
    http2_local_connection_state local_connection_state_;
    http2_role role_{http2_role::server};
    std::uint32_t next_local_stream_id_{1};  // client role: next odd stream id to open
    std::uint32_t active_local_request_streams_{0};
    std::optional<http2_peer_goaway> peer_goaway_;
    std::int32_t connection_send_window_{http2_default_initial_window_size};
    std::int32_t connection_receive_window_{
        static_cast<std::int32_t>(http2_local_settings::initial_window_size)};
    http2_receive_window_credit connection_receive_credit_;
    preface_phase_type preface_phase_{preface_phase_type::not_started};

    // Defense-in-depth flood budgets (see http2_connection.cpp). No clock in the core, so
    // these are per-connection counters that trip GOAWAY(ENHANCE_YOUR_CALM).
    std::uint32_t peer_reset_streams_{0};    // new stream aborts from peer RST_STREAM
    std::uint32_t completed_responses_{0};   // streams finished without reset (refills budget)
    std::uint32_t consecutive_pings_{0};     // inbound PINGs since output was last drained
    std::uint32_t consecutive_settings_{0};  // inbound non-ACK SETTINGS since output drained
};

}  // namespace ruvia::detail
