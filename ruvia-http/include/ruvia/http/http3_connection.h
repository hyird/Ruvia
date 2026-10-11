#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <variant>

#include "ruvia/http/http3_client_request_head.h"
#include "ruvia/http/http3_connection_error.h"
#include "ruvia/http/http3_critical_stream_output.h"
#include "ruvia/http/http3_field_section.h"
#include "ruvia/http/http3_message_body.h"
#include "ruvia/http/http3_message_head.h"
#include "ruvia/http/http3_peer_streams.h"
#include "ruvia/http/http3_qpack_connection.h"
#include "ruvia/http/http3_request_writer.h"
#include "ruvia/http/http3_response_writer.h"
#include "ruvia/http/http3_settings.h"
#include "ruvia/http/http3_var_int.h"
#include "ruvia/http/http_client_response_head.h"
#include "ruvia/http/http_connection_advertisement.h"
#include "ruvia/http/http_known_method.h"
#include "ruvia/http/http_priority.h"
#include "ruvia/http/http_push.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_response_stream.h"

namespace ruvia {

enum class http3_connection_status : std::uint8_t {
    need_more_data,
    qpack_blocked,
    push_promise_pending,
    message_end,
    stream_error,
    connection_error,
    reset,
};

enum class http3_connection_event_kind : std::uint8_t {
    push_promise,
    // A received push stream has decoded its Push ID. This can precede its
    // promise; stream_id is the physical server-initiated unidirectional stream.
    push_stream,
    push_canceled,
    priority_update,
    origin_advertisement,
    // Server: the request HEADERS section is complete and decoded. Emitted in
    // the feed() call that completes it (after QPACK unblocking, if blocked),
    // before any body, tunnel data, trailers or message_end of that stream.
    // This is the request-head boundary, e.g. for switching from a header
    // read timeout to a body read timeout.
    request_head,
    informational_head,
    final_head,
    tunnel_data,
    body,
    trailer_field,
    message_end,
    reset,
};

struct http3_connection_event final {
    http3_connection_event_kind kind_{http3_connection_event_kind::body};
    std::uint64_t stream_id_{0};
    // Head events hand the decoded head to the callee, which may move from it
    // (for example into http3_server_request); nothing reads it afterwards.
    http3_message_head* head_{nullptr};
    http3_field_section_field_view trailer_{};
    std::span<const char> body_{};
    // Client response final-head and message-end events preserve the same value.
    // Server request events and all other event kinds leave it empty.
    std::optional<http_response_body_plan> response_body_plan_{};
    std::optional<std::uint64_t> push_id_{};
    std::optional<http_priority_update> priority_update_{};
    const http_origin_advertisement* origin_advertisement_{nullptr};
    std::optional<http_client_request_content_signal> request_content_signal_{};
};

using http3_connection_callback_type = void (*)(void*, const http3_connection_event&);

struct http3_connection_config final {
    std::size_t max_active_streams_{128};
    // Peer unidirectional streams include control/QPACK and are independent
    // of the request/push concurrency budget.
    std::size_t max_peer_unidirectional_streams_{128};
    std::size_t max_field_section_size_{64 * 1024};
    std::size_t max_fields_{256};
    std::size_t max_encoded_field_section_bytes_{64 * 1024};
    // Advertise the same values in local SETTINGS. Zero disables dynamic QPACK.
    std::size_t qpack_max_table_capacity_{0};
    std::size_t qpack_blocked_streams_{0};
    // Match local SETTINGS_ENABLE_CONNECT_PROTOCOL. Ordinary CONNECT is independent.
    bool enable_connect_protocol_{false};
    bool enable_datagrams_{false};
    // Enable only for a TLS-authenticated origin connection, not an explicit proxy.
    bool receive_origin_advertisements_{false};
    // Client role: authorize pushes up to this ID; advertise MAX_PUSH_ID on control.
    std::optional<std::uint64_t> max_push_id_{};
    std::size_t max_remembered_pushes_{128};
};

// Server push stream prefix: the stream type and Push ID that open a push stream.
struct http3_push_stream_prefix final {
    std::array<char, 1 + http3_var_int_max_bytes> bytes_{};
    std::size_t size_{0};

    [[nodiscard]] std::span<const char> bytes() const& noexcept {
        return std::span<const char>(bytes_).first(size_);
    }
    std::span<const char> bytes() const&& = delete;
};

struct http3_connection_result final {
    http3_connection_status status_{http3_connection_status::need_more_data};
    http3_connection_error_scope scope_{http3_connection_error_scope::none};
    http3_connection_error_code code_{http3_connection_error_code::no_error};
    std::size_t consumed_bytes_{0};
};

// Sans-I/O receive-side HTTP/3 connection. All connection and live-request
// state uses resource and is released on connection/request termination. The
// resource must outlive this object and every callback. Event views (including
// head and trailer fields) are valid only during the synchronous callback.
// Callers must guarantee synchronous consumer capacity
// before feeding bytes; this class deliberately has no unbounded event queue.
// qpack_blocked consumes only consumed_bytes. Keep the remaining bytes and FIN,
// deliver peer encoder-stream bytes, then feed the suffix on the blocked stream.
// The completed HEADERS section is retained until it can be decoded. Other
// streams remain independently usable. This also applies to an empty suffix.
// The transport must only deliver bytes/FIN/RESET for QUIC streams that are
// still active; QUIC guarantees that terminated streams receive no further
// delivery. Callbacks must not call feed() recursively, move, or destroy this
// connection. Recursive feed throws std::logic_error without changing the
// outer feed state. Callback/allocator exceptions propagate and latch a
// connection failure: that feed cannot be resumed transactionally. Client
// response streams must be registered before feeding.
class http3_connection final {
public:
    http3_connection(http3_peer_role local_role, std::pmr::memory_resource* resource,
        http3_connection_config limits = {});
    ~http3_connection();
    http3_connection(http3_connection&&) noexcept;
    http3_connection& operator=(http3_connection&&) noexcept;
    http3_connection(const http3_connection&) = delete;
    http3_connection& operator=(const http3_connection&) = delete;

    // Client only: own ticket SETTINGS before the first peer feed or local field
    // section encoding. Queued control frames and registered requests are allowed.
    // Until the TLS outcome is supplied, outgoing QPACK uses static/literal fields.
    // Invalid values throw invalid_argument; a late/repeated or wrong-role call
    // throws logic_error. A latched connection failure is returned unchanged.
    [[nodiscard]] http3_connection_result begin_early_data(http3_settings remembered);
    // Supply the TLS outcome once, before replaying rejected requests or feeding
    // subsequent peer bytes. Rejection discards remembered SETTINGS. Acceptance
    // validates them against actual SETTINGS, whether those arrive before or
    // after this call; incompatibility returns and latches a connection error.
    // Allocator exceptions propagate and latch internal_error. Calls without a
    // preceding begin, repeated calls, and calls during feed throw logic_error.
    [[nodiscard]] http3_connection_result complete_early_data(bool accepted);

    [[nodiscard]] http3_connection_result register_client_request(std::uint64_t stream_id,
        http_known_method method);
    // Local stream cancellation: emit QPACK Stream Cancellation before releasing
    // a live request/push parser. The driver separately terminates QUIC delivery.
    [[nodiscard]] http3_connection_result cancel_request(std::uint64_t stream_id);
    // Local parser retirement, not a peer RESET and not QUIC cancellation.
    // The transport must first terminate both directions and guarantee no
    // more delivery for this stream. Cannot retire from inside feed callbacks.
    [[nodiscard]] bool retire_client_request(std::uint64_t stream_id) noexcept;
    // Server-role counterpart for an incoming request, including partial HEADERS.
    // The same transport-stop and callback restrictions apply.
    [[nodiscard]] bool retire_server_request(std::uint64_t stream_id) noexcept;
    // Permanently release all protocol storage after transport delivery stops.
    // No callbacks or wire signals are generated. Invalidates peer_settings()
    // references. False when already retired or called from a feed callback.
    [[nodiscard]] bool retire() noexcept;
    // Server: request heads completed during this call are decoded directly
    // into request_head_resource (nullptr selects the connection resource), so
    // a per-request owner can adopt the head from the request_head event.
    [[nodiscard]] http3_connection_result feed(std::uint64_t stream_id, std::span<const char> bytes,
        bool fin, bool reset, http3_connection_callback_type callback, void* context,
        std::pmr::memory_resource* request_head_resource = nullptr);
    // Server: creates a complete PUSH_PROMISE frame; caller transmits it on the
    // associated request stream. The returned bytes use this connection's resource.
    // The associated response's send half must still be open; the driver owns
    // send-side HEADERS/DATA/FIN progression through the message write plans.
    [[nodiscard]] std::variant<std::pmr::vector<char>, http3_connection_error_code> prepare_push_promise(
        std::uint64_t associated_stream_id, std::uint64_t push_id, http_push_request_view request);
    // Control frames (excluding the control stream type and SETTINGS, which
    // http3_critical_stream_output owns) are appended to this connection's
    // control output; no_error means queued. Protocol state commits only once
    // the frame is queued. Drain it with pending/consume_critical_output.
    [[nodiscard]] http3_connection_error_code queue_max_push_id(std::uint64_t maximum);
    [[nodiscard]] http3_connection_error_code queue_cancel_push(std::uint64_t push_id);
    [[nodiscard]] http3_connection_error_code queue_goaway(std::uint64_t first_unprocessed_id);
    [[nodiscard]] http3_connection_error_code queue_priority_update(http_priority_update update);
    [[nodiscard]] http3_connection_error_code queue_origin_advertisement(std::span<const std::string_view> origins);
    // Server: stream type + push ID. One server unidirectional stream per push.
    [[nodiscard]] std::variant<http3_push_stream_prefix, http3_connection_error_code> prepare_push_stream(std::uint64_t stream_id, std::uint64_t push_id);
    [[nodiscard]] std::optional<std::uint64_t> peer_max_push_id() const noexcept;
    // Validated promise metadata, borrowed until connection retirement. Available
    // to either role after preparing or receiving the complete PUSH_PROMISE.
    [[nodiscard]] const http3_message_head* promised_request(std::uint64_t push_id) const& noexcept;
    const http3_message_head* promised_request(std::uint64_t) const&& = delete;

    // Uses effective_peer_settings() to encode a QPACK section for any local
    // message or promise. Before actual SETTINGS or while the TLS early-data
    // outcome is pending, only static/literal representations are used.
    // All connection-owned encoders enforce the peer's decoded field-section
    // limit before compression, independently of local encoded-byte budgets.
    // Message helpers validate HTTP semantics; this entry point owns compression.
    [[nodiscard]] std::variant<std::pmr::vector<char>, http3_qpack_connection_error> encode_field_section(
        std::uint64_t stream_id, std::span<const http3_field_section_field_view> fields);
    [[nodiscard]] std::variant<http3_client_request_head, http3_client_request_head_failure> encode_client_request_head(
        std::uint64_t stream_id, http3_client_request_head_view view, http3_field_section_limits limits = {});
    [[nodiscard]] std::variant<http3_headers_frame, http3_request_trailer_error> encode_request_trailers(std::uint64_t stream_id,
        std::span<const http3_field_section_field_view> fields, http3_field_section_limits limits = {});
    [[nodiscard]] std::variant<http3_response_head, http3_response_head_failure> encode_connect_response_head(std::uint64_t stream_id,
        const http_response& response, http3_field_section_limits limits = {});
    [[nodiscard]] std::variant<http3_response_head, http3_response_head_failure> encode_response_head(std::uint64_t stream_id,
        const http_response& response, http_buffered_response_write_plan plan, http3_field_section_limits limits = {});
    [[nodiscard]] std::variant<http3_streaming_response_head, http3_response_head_failure> encode_streaming_response_head(std::uint64_t stream_id,
        http_response response, http_known_method method, http_response_stream_kind kind, http_response_trailer_intent trailers, http3_field_section_limits limits = {});
    [[nodiscard]] std::variant<http3_response_head, http3_response_head_failure> encode_interim_response_head(std::uint64_t stream_id,
        const http_interim_response_head& response, http3_field_section_limits limits = {});
    [[nodiscard]] std::variant<http3_headers_frame, http3_response_head_failure> encode_response_trailers(std::uint64_t stream_id,
        std::span<const http3_field_section_field_view> fields, http3_field_section_limits limits = {});

    // Bytes this connection owns for a local critical stream, after the stream
    // prefix that http3_critical_stream_output emits: queued control frames,
    // QPACK encoder instructions or QPACK decoder instructions. The span stays
    // stable until the next consume or queue/encode/feed call, so drivers
    // write straight from it and then consume exactly the accepted bytes.
    [[nodiscard]] std::span<const char> pending_critical_output(http3_critical_stream_output::stream_kind kind) const& noexcept;
    std::span<const char> pending_critical_output(http3_critical_stream_output::stream_kind) const&& = delete;
    [[nodiscard]] bool consume_critical_output(http3_critical_stream_output::stream_kind kind, std::size_t bytes) noexcept;

    [[nodiscard]] std::size_t active_request_count() const noexcept;
    // Use these exact settings to construct http3_local_critical_streams. This
    // binds advertised receive capabilities to their connection-owned state.
    [[nodiscard]] http3_settings local_settings() const noexcept;
    // Actual peer SETTINGS only; never substitutes ticket values. References
    // remain borrowed from this connection and are invalidated by retirement.
    [[nodiscard]] const std::optional<http3_settings>& peer_settings() const noexcept;
    // Sending limits/capabilities: actual SETTINGS take precedence immediately;
    // otherwise remembered SETTINGS apply until early data is rejected. Empty
    // means protocol defaults. The reference is invalidated by begin/complete,
    // feed, move or retirement; do not retain it across those operations.
    [[nodiscard]] const std::optional<http3_settings>& effective_peer_settings() const noexcept;
    [[nodiscard]] std::optional<std::uint64_t> peer_goaway_id() const noexcept;
    // Conservative RFC 9114 unprocessed evidence for a live client request.
    // Query before feeding a RESET or retiring the request. The optional code
    // must come from the peer's RESET_STREAM, never local STOP_SENDING. Any
    // observed response head (including informational) prevents classification
    // as unprocessed. This does not schedule retries or transfer request data.
    [[nodiscard]] bool peer_reports_unprocessed(std::uint64_t stream_id,
        std::optional<std::uint64_t> peer_reset_error_code = {}) const noexcept;

private:
    struct impl_type;
    std::pmr::memory_resource* resource_;
    impl_type* impl_;
};

}  // namespace ruvia
