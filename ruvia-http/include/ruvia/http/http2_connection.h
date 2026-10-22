#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>

#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/http2_event.h"
#include "ruvia/http/http2_framing.h"
#include "ruvia/http/http2_request_content.h"
#include "ruvia/http/http2_types.h"
#include "ruvia/http/http_client.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_interim_response.h"
#include "ruvia/http/http_priority.h"
#include "ruvia/http/http_protocol_error.h"
#include "ruvia/http/http_push.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_response_server.h"
#include "ruvia/http/http_response_stream.h"
#include "ruvia/http/websocket_handshake.h"

namespace ruvia {

enum class http2_server_request_release_status : std::uint8_t { released,
    closed };
struct http2_connection_options final {
    // The resource must outlive the connection and any escaped events or
    // credits, including their owned heads and trailers.
    std::pmr::memory_resource* resource_{nullptr};
    bool enable_push_{false};
    // Enable only for an authenticated h2 connection to an origin server.
    // RFC 8336 forbids using ORIGIN over h2c or through an explicit proxy.
    bool receive_origin_advertisements_{false};
};

struct http2_websocket_server_handshake_options final {
    std::span<const std::string_view> supported_subprotocols_{};
    std::span<const http_header_view> response_headers_{};
    websocket_deflate_config deflate_{};
};

struct http2_regular_request_head_view final {
    borrowed_text method_{"GET"};
    borrowed_text scheme_{"https"};
    std::optional<borrowed_text> authority_{};
    borrowed_text target_{"/"};
    std::span<const http_header_view> headers_{};
    http2_request_content content_{http2_request_content::none()};
    http_client_request_expectation expectation_{http_client_request_expectation::none};
};

struct http2_connect_request_head_view final {
    borrowed_text authority_{};
    std::span<const http_header_view> headers_{};
};

struct http2_extended_connect_request_head_view final {
    borrowed_text protocol_{};
    borrowed_text scheme_{"https"};
    borrowed_text authority_{};
    borrowed_text target_{"/"};
    std::span<const http_header_view> headers_{};
};

// Stable HTTP/2 sans-I/O driver. Feed transport bytes, drain typed events, and
// flush pending_output(); the class owns all HPACK, stream and flow-control state.
// The storage behind an accepted feed() input must remain alive and unchanged
// until every event produced by that call has been taken and every event-derived
// byte view has expired. Calling feed() again may invalidate those views; owning
// string temporaries are rejected at compile time.
// Event, deferred-DATA and drain-notification queue capacities grow geometrically
// and are retained for reuse; their storage uses the connection's PMR resource.
// Receive-field text growth reserves the whole name/value pair before copying.
// Request field capacity includes synthesized Host and coalesced Cookie. A head
// that cannot fit the public request view resets its stream before publication.
// Outbound HPACK never inserts dynamic entries; required capacity reductions select zero.
// A fatal connection error rejects new stream output. Request leases and DATA
// credits remain releasable without generating more frames. Graceful local or
// peer GOAWAY still permits output on established streams that remain open.
// A dropped lease or credit whose release fails is retried by the next feed(),
// next_event(), output drain, submit_data(), finish_request(), or submit_reset();
// const queries never mutate.
// For a streaming client request, response END_STREAM closes only the receive
// half. Finish or explicitly reset the request send half even after message_end.
// Informational heads do not end the receive half and have no fixed count limit;
// each header section remains subject to the configured decoding limits.
// Received 1xx fields remain ordered metadata; Content-Length and Trailer do
// not frame the interim or final response. END_STREAM on a 1xx remains malformed.
// Received Trailer declarations remain opaque metadata in requests and responses;
// they never authorize forbidden actual trailer fields or select message framing.
// Generic-byte-valid Expect members are classified for the owner's existing
// 417/ignore policy, not rejected as malformed merely for member syntax.
// Received request TE permits empty lists and ignores empty members; every
// nonempty member must be "trailers" (case-insensitive).
// Response TE remains malformed even when empty.
// An incoming OPTIONS body does not require Content-Type at admission; RFC 9110
// section 9.3.7 places that media-description requirement on senders.
// TRACE accepts zero Content-Length and empty DATA endings but rejects content.
class http2_connection final {
public:
    [[nodiscard]] static http2_connection server(http2_connection_options options = {});
    [[nodiscard]] static http2_connection client(http2_connection_options options = {});
    ~http2_connection();
    http2_connection(const http2_connection&) = delete;
    http2_connection& operator=(const http2_connection&) = delete;
    http2_connection(http2_connection&&) noexcept;
    http2_connection& operator=(http2_connection&&) noexcept;

    [[nodiscard]] http2_role role() const noexcept;
    [[nodiscard]] bool received_peer_settings() const noexcept;
    // Read-only observation for transport timeout decisions while a CONTINUATION
    // sequence is incomplete.
    [[nodiscard]] bool header_block_in_progress() const noexcept;
    [[nodiscard]] http2_feed_result feed(std::string_view input);
    template <detail::http_temporary_owning_char_string input_type>
    http2_feed_result feed(input_type&&) = delete;
    [[nodiscard]] std::optional<http2_event> next_event();
    [[nodiscard]] std::string_view pending_output() const& noexcept;
    std::string_view pending_output() const&& = delete;
    [[nodiscard]] http2_output_consume_status consume_output(std::size_t bytes);
    void take_output(std::pmr::string& output);
    // Appends whole frame boundaries (the first frame may exceed max_bytes).
    // A complete unconsumed batch swaps storage when output is empty and its
    // allocator matches the connection; all other cases copy the selected bytes.
    // On copy failure the output cursor and observer are untouched. After legacy
    // partial-byte consume_output(), a cursor inside a segment returns unaligned;
    // advance to its next boundary or drain the suffix with take_output().
    // A client preface is treated as one indivisible segment.
    [[nodiscard]] http2_output_batch_result take_output_batch(std::size_t max_bytes,
        std::pmr::string& output, http2_data_output_observer_type observer = nullptr,
        void* observer_context = nullptr);
    [[nodiscard]] bool wants_write() const noexcept;

    // Submitted fields are encoded/owned before returning. If owner-lease
    // allocation fails after HEADERS commit, cancellation follows the deferred
    // release rules above and the original exception is rethrown.
    [[nodiscard]] std::variant<std::uint32_t, http2_request_head_submit_error> submit_request_head(
        const http2_regular_request_head_view& request);
    [[nodiscard]] std::variant<std::uint32_t, http2_request_head_submit_error> submit_request_head(
        const http2_connect_request_head_view& request);
    [[nodiscard]] std::variant<std::uint32_t, http2_request_head_submit_error> submit_request_head(
        const http2_extended_connect_request_head_view& request);
    // Bytes may borrow pending_output(); such input is copied in the connection
    // resource before output growth can invalidate that source.
    [[nodiscard]] http2_data_submit_status submit_data(
        std::uint32_t stream_id, std::string_view bytes, http2_end_stream end_stream);
    [[nodiscard]] http2_finish_request_status finish_request(std::uint32_t stream_id,
        std::span<const http_header_view> trailers = {});
    [[nodiscard]] http2_request_content_release_status release_request_content(
        std::uint32_t stream_id) noexcept;
    // Commits a promise and returns the server request/storage lease used to
    // produce its response. Release or abandon it exactly like an incoming request.
    // If materializing the lease fails after promise commit, the promised stream
    // is cancelled using the deferred release rules above; the parent is unchanged.
    [[nodiscard]] std::variant<http2_request_head_event, http2_push_submit_error> submit_push_request(
        std::uint32_t associated_stream_id, http_push_request_view request);

    [[nodiscard]] http2_submit_status submit_interim_response_head(
        std::uint32_t stream_id, const http_interim_response_head& response);
    [[nodiscard]] http2_submit_status submit_connect_response_head(
        std::uint32_t stream_id, const http_response& response);
    [[nodiscard]] std::variant<http2_websocket_negotiation, http2_websocket_handshake_submit_error>
    submit_websocket_handshake(std::uint32_t stream_id, const http_request& request,
        const websocket_handshake_validation_result& validation,
        http2_websocket_server_handshake_options options = {});
    // Submit only the final buffered-response head. The committed plan tells the
    // caller whether/how much representation remains for DATA or file output.
    [[nodiscard]] std::variant<http_buffered_response_write_plan, http2_response_head_submit_error>
    submit_response_head(std::uint32_t stream_id, const http_response& response,
        http_buffered_response_write_plan write_plan);
    // Submit a final generic response head and leave an eligible response body
    // open for subsequent submit_data() calls. Content-Length is not generated
    // automatically; an explicit value, when present, constrains the total
    // submitted DATA bytes.
    [[nodiscard]] std::variant<http_response_stream_commit_plan, http2_response_head_submit_error>
    submit_streaming_response_head(std::uint32_t stream_id, http_response response,
        http_response_trailer_intent trailer_intent = http_response_trailer_intent::none);
    // Encodes trailers before returning accepted or queued; field storage may be
    // released or changed even while the ending waits for DATA flow control.
    [[nodiscard]] http2_finish_response_status finish_response(
        std::uint32_t stream_id, const http_response_trailer_section& trailers);
    [[nodiscard]] http2_submit_status submit_origin_advertisement(std::span<const std::string_view> origins);
    [[nodiscard]] http2_submit_status submit_alternative_service_advertisement(std::uint32_t stream_id,
        std::string_view origin, std::string_view field_value);
    [[nodiscard]] http2_submit_status submit_priority_update(std::uint32_t stream_id, http_priority_fields fields);

    // Resetting a stream does not return connection send credit or reopen other
    // streams' send windows.
    // Resetting an associated request leaves any in-progress PUSH_PROMISE block
    // intact; its CONTINUATION can still publish a promised-stream event.
    [[nodiscard]] http2_submit_status submit_reset(std::uint32_t stream_id, http2_error_code error);
    [[nodiscard]] http2_received_data_acknowledge_status acknowledge(http2_received_data_credit&& credit);
    [[nodiscard]] bool has_queued_data(std::uint32_t stream_id) const noexcept;
    // Per-stream protocol ownership of submitted DATA: queued means the core still
    // owns a flow-control-blocked suffix; drained means none remains; aborted means
    // the stream reset/closed and queued DATA was discarded.
    [[nodiscard]] http2_data_queue_state data_queue_state(std::uint32_t stream_id) const noexcept;
    // Payload bytes in serialized DATA frames still owned by the core output buffer.
    [[nodiscard]] std::size_t pending_data_output_bytes(std::uint32_t stream_id) const noexcept;
    // Read-only state used by a response writer to decide whether a send-window
    // wait can make progress. Missing/closed streams have no window snapshot.
    [[nodiscard]] std::optional<http2_send_window_state> send_window_state(
        std::uint32_t stream_id) const noexcept;
    // Missing streams are considered aborted, making the query safe during teardown.
    [[nodiscard]] bool stream_aborted(std::uint32_t stream_id) const noexcept;
    // Reports the protocol receive state without exposing internal stream storage.
    [[nodiscard]] http2_stream_receive_status stream_receive_status(
        std::uint32_t stream_id) const noexcept;
    // Reports each queued-send drain once, even if its final DATA closed the stream.
    // Control frames with no queued DATA preserve any untaken drain notifications.
    // A reset cancels untaken notifications. IDs borrow storage until the next
    // take_drained_data_streams() call or connection destruction.
    [[nodiscard]] std::span<const std::uint32_t> take_drained_data_streams() & noexcept;
    std::span<const std::uint32_t> take_drained_data_streams() && = delete;
    // Consume the server request event only after every request-derived view has
    // expired. An event cannot be released through another connection.
    [[nodiscard]] http2_server_request_release_status release(http2_request_head_event&& request);
    void begin_drain();
    [[nodiscard]] bool draining() const noexcept;
    [[nodiscard]] std::optional<http2_error_code> connection_error() const noexcept;

private:
    friend websocket_handshake_validation_result validate_http2_websocket_handshake(
        http2_connection&, std::uint32_t, const http_request&,
        websocket_handshake_validation_options) noexcept;
    explicit http2_connection(std::pmr::memory_resource* resource, http2_role role, bool enable_push, bool receive_origin_advertisements);
    class impl_type;
    struct impl_deleter_type final {
        void operator()(impl_type* value) const noexcept;
    };
    std::unique_ptr<impl_type, impl_deleter_type> impl_;
};
// Validates an RFC 8441 extended-CONNECT handshake against the decoded stream
// state and its semantic request. The request and connection must remain alive
// for the duration of the call.
[[nodiscard]] websocket_handshake_validation_result validate_http2_websocket_handshake(
    http2_connection& connection, std::uint32_t stream_id,
    const http_request& request,
    websocket_handshake_validation_options options = {}) noexcept;

}  // namespace ruvia
