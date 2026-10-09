#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <variant>

#include "ruvia/http/http3_client_request_head.h"
#include "ruvia/http/http3_connection_error.h"
#include "ruvia/http/http3_field_section.h"
#include "ruvia/http/http3_message_body.h"
#include "ruvia/http/http3_message_head.h"
#include "ruvia/http/http3_peer_streams.h"
#include "ruvia/http/http3_qpack_connection.h"
#include "ruvia/http/http3_response_writer.h"
#include "ruvia/http/http3_settings.h"
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
    const http3_message_head* head_{nullptr};
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
    [[nodiscard]] http3_connection_result feed(std::uint64_t stream_id, std::span<const char> bytes,
        bool fin, bool reset, http3_connection_callback_type callback, void* context);
    // Server: creates a complete PUSH_PROMISE frame; caller transmits it on the
    // associated request stream. The returned bytes use this connection's resource.
    // The associated response's send half must still be open; the driver owns
    // send-side HEADERS/DATA/FIN progression through the message write plans.
    [[nodiscard]] std::variant<std::pmr::vector<char>, http3_connection_error_code> prepare_push_promise(
        std::uint64_t associated_stream_id, std::uint64_t push_id, http_push_request_view request);
    // Complete control frames, excluding the control stream type and SETTINGS.
    // Queue the returned bytes in order. Protocol state commits at preparation.
    [[nodiscard]] std::variant<std::pmr::vector<char>, http3_connection_error_code> prepare_max_push_id(std::uint64_t maximum);
    [[nodiscard]] std::variant<std::pmr::vector<char>, http3_connection_error_code> prepare_cancel_push(std::uint64_t push_id);
    [[nodiscard]] std::variant<std::pmr::vector<char>, http3_connection_error_code> prepare_goaway(std::uint64_t first_unprocessed_id);
    [[nodiscard]] std::variant<std::pmr::vector<char>, http3_connection_error_code> prepare_priority_update(http_priority_update update);
    // Server: stream type + push ID. One server unidirectional stream per push.
    [[nodiscard]] std::variant<std::pmr::vector<char>, http3_connection_error_code> prepare_push_stream(std::uint64_t stream_id, std::uint64_t push_id);
    [[nodiscard]] std::variant<std::pmr::vector<char>, http3_connection_error_code> prepare_origin_advertisement(
        std::span<const std::string_view> origins);
    [[nodiscard]] std::optional<std::uint64_t> peer_max_push_id() const noexcept;
    // Validated promise metadata, borrowed until connection retirement. Available
    // to either role after preparing or receiving the complete PUSH_PROMISE.
    [[nodiscard]] const http3_message_head* promised_request(std::uint64_t push_id) const& noexcept;
    const http3_message_head* promised_request(std::uint64_t) const&& = delete;

    // Uses peer SETTINGS to encode a QPACK section for any local message or
    // promise. Before SETTINGS, only static/literal representations are used.
    // All connection-owned encoders enforce the peer's decoded field-section
    // limit before compression, independently of local encoded-byte budgets.
    // Message helpers validate HTTP semantics; this entry point owns compression.
    [[nodiscard]] std::variant<std::pmr::vector<char>, http3_qpack_connection_error> encode_field_section(
        std::uint64_t stream_id, std::span<const http3_field_section_field_view> fields);
    [[nodiscard]] std::variant<http3_client_request_head, http3_client_request_head_failure> encode_client_request_head(
        std::uint64_t stream_id, http3_client_request_head_view view, http3_field_section_limits limits = {});
    [[nodiscard]] std::variant<http3_response_head, http3_response_head_failure> encode_connect_response_head(std::uint64_t stream_id,
        const http_response& response, http3_field_section_limits limits = {});
    [[nodiscard]] std::variant<http3_response_head, http3_response_head_failure> encode_response_head(std::uint64_t stream_id,
        const http_response& response, http_buffered_response_write_plan plan, http3_field_section_limits limits = {});
    [[nodiscard]] std::variant<http3_streaming_response_head, http3_response_head_failure> encode_streaming_response_head(std::uint64_t stream_id,
        http_response response, http_known_method method, http_response_stream_kind kind, http_response_trailer_intent trailers, http3_field_section_limits limits = {});
    [[nodiscard]] std::variant<http3_response_head, http3_response_head_failure> encode_interim_response_head(std::uint64_t stream_id,
        const http_interim_response_head& response, http3_field_section_limits limits = {});
    [[nodiscard]] std::variant<http3_response_field_section, http3_response_head_failure> encode_response_trailers(std::uint64_t stream_id,
        std::span<const http3_field_section_field_view> fields, http3_field_section_limits limits = {});
    [[nodiscard]] std::span<const char> pending_qpack_encoder_output() const& noexcept;
    std::span<const char> pending_qpack_encoder_output() const&& = delete;
    [[nodiscard]] bool consume_qpack_encoder_output(std::size_t bytes) noexcept;

    // Decoder instructions (without stream-type prefix) belong on the local
    // QPACK decoder critical stream. Consume only successfully transmitted bytes.
    [[nodiscard]] std::span<const char> pending_qpack_decoder_output() const& noexcept;
    std::span<const char> pending_qpack_decoder_output() const&& = delete;
    [[nodiscard]] bool consume_qpack_decoder_output(std::size_t bytes) noexcept;

    [[nodiscard]] std::size_t active_request_count() const noexcept;
    // Use these exact settings to construct http3_local_critical_streams. This
    // binds advertised receive capabilities to their connection-owned state.
    [[nodiscard]] http3_settings local_settings() const noexcept;
    [[nodiscard]] const std::optional<http3_settings>& peer_settings() const noexcept;
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
