#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "ruvia/http/http_interim_response.h"
#include "ruvia/http/http_known_method.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_response_server.h"
#include "ruvia/http/http_response_stream.h"

namespace ruvia {
class http3_quic_connection;

enum class http3_role : std::uint8_t { client,
    server };

// HTTP/3 application error codes a server sends (RFC 9114 §8.1), for
// RESET_STREAM, STOP_SENDING and CONNECTION_CLOSE.
enum class http3_error_code : std::uint64_t {
    no_error = 0x0100,
    internal_error = 0x0102,
    request_rejected = 0x010b,
    request_cancelled = 0x010c,
    message_error = 0x010e,
};

enum class http3_status : std::uint8_t {
    accepted,
    would_block,
    events_pending,
    closed,
    invalid_state,
    protocol_error,
};

struct http3_result final {
    http3_status status_{http3_status::accepted};
    // Zero unless status_ is protocol_error. The value is the stable nghttp3
    // application/library error code suitable for a QUIC CONNECTION_CLOSE.
    std::uint64_t error_code_{0};
    // Stream bytes that the QUIC owner can return as receive flow credit.
    std::size_t consumed_bytes_{0};
};

struct http3_field_view final {
    std::string_view name_;
    std::string_view value_;
    bool sensitive_{false};
};

class http3_field final {
public:
    http3_field(std::string_view name, std::string_view value,
        std::pmr::memory_resource* resource);

    [[nodiscard]] std::string_view name() const noexcept {
        return name_;
    }
    [[nodiscard]] std::string_view value() const noexcept {
        return value_;
    }

private:
    std::pmr::string name_;
    std::pmr::string value_;
};

// One decoded field section: the initial request or response head, or a
// trailer section (trailers_) when the stream already carried one.
struct http3_headers_event final {
    std::int64_t stream_id_{-1};
    std::pmr::vector<http3_field> fields_;
    bool stream_ended_{false};
    bool trailers_{false};
};

// DATA payload. The QUIC owner returns its flow-control credit only when the
// application consumes it (http3_quic_connection::consume_stream_data).
struct http3_data_event final {
    http3_data_event(std::int64_t stream_id, std::string_view bytes,
        std::pmr::memory_resource* resource, bool own_bytes);
    http3_data_event(const http3_data_event&) = delete;
    http3_data_event& operator=(const http3_data_event&) = delete;
    http3_data_event(http3_data_event&& other) noexcept;
    http3_data_event& operator=(http3_data_event&& other);

    std::int64_t stream_id_{-1};
    std::string_view bytes_;

private:
    std::pmr::string owned_bytes_;
    bool owns_bytes_{false};
};

struct http3_stream_end_event final {
    std::int64_t stream_id_{-1};
};

enum class http3_transport_action : std::uint8_t {
    stop_sending,
    reset_stream,
};

struct http3_transport_action_event final {
    std::int64_t stream_id_{-1};
    std::uint64_t application_error_code_{0};
    http3_transport_action action_{http3_transport_action::reset_stream};
};

struct http3_deferred_consume_event final {
    std::int64_t stream_id_{-1};
    std::size_t bytes_{0};
};

// The stream is closed in both directions; no further event names it and its
// identifier may not be submitted to again. An error code is present when the
// stream ended by RESET_STREAM or STOP_SENDING rather than by completing.
struct http3_stream_close_event final {
    std::int64_t stream_id_{-1};
    std::optional<std::uint64_t> application_error_code_;
};

// The peer sent GOAWAY (RFC 9114 §5.2). For a client, id_ is the first request
// stream the server will not process; NGHTTP3_SHUTDOWN_NOTICE_STREAM_ID marks
// a shutdown notice that does not yet bound any stream.
struct http3_goaway_event final {
    std::int64_t id_{-1};
};

using http3_event = std::variant<http3_headers_event, http3_data_event,
    http3_stream_end_event, http3_transport_action_event,
    http3_deferred_consume_event, http3_stream_close_event, http3_goaway_event>;

struct http3_stream_output final {
    [[nodiscard]] std::size_t size() const noexcept;

    std::int64_t stream_id_{-1};
    std::array<std::span<const std::byte>, 16> buffers_{};
    std::size_t buffer_count_{0};
    bool fin_{false};
};

// The accepted prefix of a submit_data() call. The codec copies exactly that
// prefix; the caller keeps the rest and retries after acknowledgements free
// output budget (pending_output_bytes()).
struct http3_data_result final {
    http3_status status_{http3_status::accepted};
    std::size_t accepted_bytes_{0};
};

enum class http3_response_head_error : std::uint8_t {
    // The stream no longer exists or the connection failed.
    closed,
    // A final head was already submitted, or the role is not server.
    invalid_state,
    // The response cannot be represented in HTTP/3 (RFC 9114 §4.2:
    // connection-specific fields, a non-final status, an invalid
    // Content-Length or Transfer-Encoding).
    invalid_message,
    // The write plan does not describe this response.
    response_plan_mismatch,
};

struct http3_connection_options final {
    std::pmr::memory_resource* resource_{nullptr};
    std::uint64_t max_field_section_size_{64 * 1024};
    std::size_t max_concurrent_streams_{100};
    std::uint64_t qpack_max_dtable_capacity_{4096};
    std::uint64_t qpack_blocked_streams_{16};
    // Upper bound on DATA payload one stream holds between submit_data() and
    // the peer's acknowledgement. submit_data() accepts only what fits, so a
    // stalled peer cannot make the codec queue unbounded output.
    std::size_t max_stream_output_bytes_{256 * 1024};
    // Server SETTINGS_ENABLE_CONNECT_PROTOCOL (RFC 9220 §3). Without it a
    // :protocol pseudo-header is a malformed request.
    bool enable_connect_protocol_{false};
    // Copies DATA into event-owned PMR storage. QUIC enables this because its
    // decrypted stream callback storage expires before receive_datagram returns.
    bool own_data_events_{false};
};

// HTTP/3 stream codec. QUIC owns stream identifiers, retransmission, flow
// control and packet I/O; this class owns QPACK and HTTP/3 control/request
// streams through nghttp3. With own_data_events_ disabled, input DATA views
// remain valid until next_event() has drained every event from the
// corresponding read_stream() call.
class http3_connection final {
public:
    [[nodiscard]] static http3_connection client(
        http3_connection_options options = {});
    [[nodiscard]] static http3_connection server(
        http3_connection_options options = {});

    ~http3_connection();
    http3_connection(const http3_connection&) = delete;
    http3_connection& operator=(const http3_connection&) = delete;
    http3_connection(http3_connection&&) noexcept;
    http3_connection& operator=(http3_connection&&) noexcept;

    [[nodiscard]] http3_role role() const noexcept;

    // Bind three locally opened unidirectional QUIC streams exactly once.
    [[nodiscard]] http3_status bind_control_streams(
        std::int64_t control_stream_id, std::int64_t qpack_encoder_stream_id,
        std::int64_t qpack_decoder_stream_id);

    // ts_nanoseconds uses one monotonic clock and never decreases.
    [[nodiscard]] http3_result read_stream(std::int64_t stream_id,
        std::span<const std::byte> bytes, bool fin, std::uint64_t ts_nanoseconds);
    [[nodiscard]] std::optional<http3_event> next_event();

    // Borrows at most max_bytes of one HTTP/3 stream without advancing
    // nghttp3. The buffers remain valid through commit_output() and QUIC keeps
    // the underlying bytes alive until acknowledge_output().
    // QUIC reports exactly the accepted prefix through commit_output().
    [[nodiscard]] http3_result peek_output(
        std::size_t max_bytes, http3_stream_output& output);
    [[nodiscard]] http3_status commit_output(
        std::int64_t stream_id, std::size_t bytes) noexcept;
    [[nodiscard]] http3_status acknowledge_output(
        std::int64_t stream_id, std::uint64_t bytes) noexcept;
    [[nodiscard]] http3_status unblock_stream(std::int64_t stream_id) noexcept;

    // QUIC allocates client-initiated bidirectional request stream identifiers.
    // end_stream=false installs a pull source: submit_data()/finish_stream()
    // resume it without retaining caller views.
    [[nodiscard]] http3_status submit_request(std::int64_t stream_id,
        std::span<const http3_field_view> fields, bool end_stream);
    [[nodiscard]] http3_status submit_response(std::int64_t stream_id,
        std::span<const http3_field_view> fields, bool end_stream);
    // Copies the prefix that fits the stream's output budget. A stream whose
    // head declared a content length rejects DATA beyond it with
    // std::length_error before copying anything.
    [[nodiscard]] http3_data_result submit_data(
        std::int64_t stream_id, std::span<const std::byte> bytes);
    // DATA payload submitted on the stream and not yet acknowledged.
    [[nodiscard]] std::size_t pending_output_bytes(
        std::int64_t stream_id) const noexcept;
    // Ends the content. A declared content length that the submitted DATA
    // does not reach throws std::length_error and leaves the stream open.
    [[nodiscard]] http3_status finish_stream(std::int64_t stream_id);
    [[nodiscard]] http3_status submit_trailers(std::int64_t stream_id,
        std::span<const http3_field_view> fields);

    // Server response mapping (RFC 9114 §4.1-§4.3). The final head carries
    // :status, the response fields with lowercase names (§4.2), a generated
    // Date when the response has none, and the Content-Length the plan
    // derives; connection-specific fields are rejected. A head whose plan
    // sends content leaves the stream open for submit_data(), whose total
    // must equal the planned length before finish_stream().
    [[nodiscard]] std::variant<http_buffered_response_write_plan, http3_response_head_error>
    submit_response_head(std::int64_t stream_id, const http_response& response,
        http_buffered_response_write_plan write_plan);
    // Streaming final head: an application Content-Length becomes the exact
    // content total; otherwise the content is unbounded. A body-suppressed
    // response (HEAD, 204, 304) ends the stream with its head.
    [[nodiscard]] std::variant<http_response_stream_commit_plan, http3_response_head_error>
    submit_streaming_response_head(std::int64_t stream_id, http_known_method request_method,
        http_response head, http_response_trailer_intent trailer_intent);
    // A 1xx response before the final head (RFC 9114 §4.1).
    [[nodiscard]] http3_status submit_interim_response_head(
        std::int64_t stream_id, const http_interim_response_head& response);
    // Ends a streaming response: an empty section ends the content, any other
    // section is sent as the trailing HEADERS frame.
    [[nodiscard]] http3_status finish_response(std::int64_t stream_id,
        const http_response_trailer_section& trailers);

    [[nodiscard]] http3_status close_stream(std::int64_t stream_id,
        std::optional<std::uint64_t> receive_error,
        std::optional<std::uint64_t> send_error) noexcept;
    // Server graceful shutdown, RFC 9114 §5.2: begin_drain() sends the
    // shutdown-notice GOAWAY that still admits in-flight requests;
    // finish_drain(), at least one round trip later, sends the final GOAWAY
    // naming the first request stream that will not be processed. drained()
    // then reports that every admitted request stream has closed.
    [[nodiscard]] http3_status begin_drain() noexcept;
    [[nodiscard]] http3_status finish_drain() noexcept;
    [[nodiscard]] bool drain_final() const noexcept;
    [[nodiscard]] bool drained() const noexcept;
    [[nodiscard]] std::optional<std::uint64_t> connection_error() const noexcept;

private:
    friend class http3_quic_connection;
    void begin_packet_input() noexcept;
    void end_packet_input() noexcept;
    [[nodiscard]] bool events_pending() const noexcept;
    [[nodiscard]] http3_status block_stream(std::int64_t stream_id) noexcept;
    [[nodiscard]] http3_status shutdown_stream_read(
        std::int64_t stream_id) noexcept;
    [[nodiscard]] http3_status shutdown_stream_write(
        std::int64_t stream_id) noexcept;
    void set_max_client_streams_bidi(std::uint64_t max_streams) noexcept;
    class impl;
    explicit http3_connection(http3_role role,
        const http3_connection_options& options);
    std::unique_ptr<impl> impl_;
};

}  // namespace ruvia
