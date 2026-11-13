#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/http2_framing.h"
#include "ruvia/http/http_client.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_protocol_error.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_request_target.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_response_server.h"
#include "ruvia/http/websocket_handshake.h"

namespace ruvia {

class http2_connection;
namespace detail {
class http2_connection;
class http2_connection_owner_endpoint;
}  // namespace detail

class http2_received_data_credit final {
public:
    // A credit is linear and has one return path: destroying a still-valid
    // token returns its exact byte count to the issuing connection's stream and
    // connection receive windows (queuing any WINDOW_UPDATE) without throwing.
    // The token carries its connection, so it cannot be returned through
    // another one, and it keeps that connection's state alive, so it may
    // outlive the facade. Returning credit for a stream that is already gone
    // is a no-op. A return whose release fails is deferred and retried as
    // described on http2_connection.
    ~http2_received_data_credit();
    http2_received_data_credit(const http2_received_data_credit&) = delete;
    http2_received_data_credit& operator=(const http2_received_data_credit&) = delete;
    http2_received_data_credit(http2_received_data_credit&& other) noexcept;
    http2_received_data_credit& operator=(http2_received_data_credit&&) = delete;
    // Combine credits from one stream without returning any bytes. Success
    // consumes other; invalid/mismatched/overflowing tokens throw.
    void merge(http2_received_data_credit&& other);
    [[nodiscard]] bool valid() const noexcept {
        return endpoint_ != nullptr && stream_id_ != 0 && bytes_ != 0;
    }

private:
    friend class http2_connection;
    friend class http2_message_body_chunk_event;
    friend class http2_tunnel_data_event;
    http2_received_data_credit(detail::http2_connection_owner_endpoint* endpoint, std::uint32_t stream_id,
        std::uint32_t bytes_value) noexcept;
    detail::http2_connection_owner_endpoint* endpoint_{nullptr};
    std::uint32_t stream_id_{0};
    std::uint32_t bytes_{0};
};

class http2_informational_head_event final {
public:
    [[nodiscard]] std::uint32_t stream_id() const noexcept {
        return stream_id_;
    }
    [[nodiscard]] const http_client_response_head& head() const& noexcept RUVIA_LIFETIMEBOUND {
        return head_;
    }
    const http_client_response_head& head() const&& = delete;
    [[nodiscard]] std::optional<http_client_request_content_signal> request_content_signal()
        const noexcept {
        return signal_;
    }

private:
    friend class detail::http2_connection;
    http2_informational_head_event(std::uint32_t stream_id, http_client_response_head head,
        std::optional<http_client_request_content_signal> signal) noexcept
        : stream_id_(stream_id),
          head_(std::move(head)),
          signal_(signal) {}
    std::uint32_t stream_id_;
    http_client_response_head head_;
    std::optional<http_client_request_content_signal> signal_;
};

// The request-head event is also the stream-storage lease for every view in
// request(). Once processing finishes, release(std::move(event)) consumes the
// event and invalidates those views as one operation.
// content_ follows the core's current receive state when the event is taken,
// so content that already arrived in the same input batch reads as no_content
// (RFC 9110 §10.1.1 then omits a pointless 100 Continue). body_open_ is fixed
// when the head is published: the HEADERS frame left the peer send half open
// and message content events follow this head, whether or not the core has
// already parsed them.
struct http2_server_request_snapshot final {
    http_request_content_indication content_{http_request_content_indication::no_content};
    std::optional<std::size_t> declared_content_length_{};
    std::optional<http_priority_fields> priority_update_{};
    bool body_open_{false};
    bool connect_pending_{false};
    // Captured from the initial HEADERS, including CONNECT; never changes as DATA arrives.
    bool initial_end_stream_{false};
};

class http2_request_head_event final {
public:
    ~http2_request_head_event();
    http2_request_head_event(const http2_request_head_event&) = delete;
    http2_request_head_event& operator=(const http2_request_head_event&) = delete;
    http2_request_head_event(http2_request_head_event&& other) noexcept;
    http2_request_head_event& operator=(http2_request_head_event&&) = delete;
    [[nodiscard]] std::uint32_t stream_id() const noexcept {
        return stream_id_;
    }
    [[nodiscard]] const http2_server_request_snapshot& snapshot() const& noexcept RUVIA_LIFETIMEBOUND {
        return snapshot_;
    }
    const http2_server_request_snapshot& snapshot() const&& = delete;
    [[nodiscard]] http2_server_request_view request_view() const& noexcept RUVIA_LIFETIMEBOUND;
    http2_server_request_view request_view() const&& = delete;
    // Head fields borrow this lease; body borrows its caller-owned storage.
    [[nodiscard]] http_request build_request(
        borrowed_text body = {}) const& RUVIA_LIFETIMEBOUND;
    http_request build_request(
        borrowed_text body = {}) const&& = delete;
    [[nodiscard]] http_server_expectation_plan expectation_plan(
        http_unsupported_expectation_policy policy) const noexcept {
        return expectations_.server_plan(content_, policy);
    }

private:
    friend class http2_connection;
    friend class http2_event;
    http2_request_head_event(detail::http2_connection_owner_endpoint* endpoint, std::uint32_t stream_id,
        http_request_expectations expectations,
        http_request_content_indication content, http2_server_request_snapshot snapshot) noexcept;
    std::uint32_t stream_id_;
    http2_server_request_snapshot snapshot_;
    http_request_expectations expectations_;
    http_request_content_indication content_;
    detail::http2_connection_owner_endpoint* endpoint_{nullptr};
};

class http2_response_head_event final {
public:
    [[nodiscard]] std::uint32_t stream_id() const noexcept {
        return stream_id_;
    }
    [[nodiscard]] const http_client_response_head& head() const& noexcept RUVIA_LIFETIMEBOUND {
        return head_;
    }
    const http_client_response_head& head() const&& = delete;
    [[nodiscard]] http_client_response_head take_head() && noexcept {
        return std::move(head_);
    }
    [[nodiscard]] std::optional<http_client_request_content_signal> request_content_signal()
        const noexcept {
        return signal_;
    }
    // The final response HEADERS carried END_STREAM: no content or tunnel DATA
    // follows this head. Fixed when the head is published; a later frame of the
    // same input batch that ends the stream (DATA with END_STREAM) never sets it.
    [[nodiscard]] bool stream_ended() const noexcept {
        return stream_ended_;
    }

private:
    friend class http2_event;
    http2_response_head_event(std::uint32_t stream_id, http_client_response_head head,
        std::optional<http_client_request_content_signal> signal, bool stream_ended) noexcept
        : stream_id_(stream_id),
          head_(std::move(head)),
          signal_(signal),
          stream_ended_(stream_ended) {}
    std::uint32_t stream_id_;
    http_client_response_head head_;
    std::optional<http_client_request_content_signal> signal_;
    bool stream_ended_;
};

class http2_message_body_chunk_event final {
public:
    [[nodiscard]] std::uint32_t stream_id() const noexcept {
        return stream_id_;
    }
    [[nodiscard]] std::string_view bytes() const noexcept {
        return bytes_;
    }
    [[nodiscard]] http2_received_data_credit take_credit() & noexcept {
        return std::move(credit_);
    }

private:
    friend class http2_event;
    http2_message_body_chunk_event(detail::http2_connection_owner_endpoint* endpoint,
        std::uint32_t stream_id, std::string_view bytes_value, std::uint32_t credit) noexcept
        : stream_id_(stream_id),
          bytes_(bytes_value),
          credit_(endpoint, stream_id, credit) {}
    std::uint32_t stream_id_;
    std::string_view bytes_;
    http2_received_data_credit credit_;
};

enum class http2_message_content_semantics : std::uint8_t {
    content,
    metadata_only,
};

// The peer ended its send half normally. trailers() is the trailer section that
// preceded the end, empty when there was none: lowercase field names (RFC 9113
// §8.2.2); a client receives them with the fields RFC 9110 §6.5.1 forbids in
// response trailers already discarded, and a server passes them to
// http_request_trailers::append_received(). The span borrows the event: it is
// invalidated when the event is moved or destroyed, or once take_trailers()
// moved the fields out.
class http2_message_end_event final {
public:
    [[nodiscard]] std::uint32_t stream_id() const noexcept {
        return stream_id_;
    }
    [[nodiscard]] std::span<const http_header> trailers() const& noexcept RUVIA_LIFETIMEBOUND {
        return trailers_;
    }
    std::span<const http_header> trailers() const&& = delete;
    [[nodiscard]] std::vector<http_header> take_trailers() && noexcept {
        return std::move(trailers_);
    }
    [[nodiscard]] http2_message_content_semantics content_semantics() const noexcept {
        return content_semantics_;
    }

private:
    friend class http2_event;
    http2_message_end_event(std::uint32_t stream_id,
        std::vector<http_header> trailers,
        http2_message_content_semantics content_semantics) noexcept
        : stream_id_(stream_id),
          trailers_(std::move(trailers)),
          content_semantics_(content_semantics) {}
    std::uint32_t stream_id_{0};
    std::vector<http_header> trailers_;
    http2_message_content_semantics content_semantics_{http2_message_content_semantics::content};
};

class http2_tunnel_data_event final {
public:
    [[nodiscard]] std::uint32_t stream_id() const noexcept {
        return stream_id_;
    }
    [[nodiscard]] std::string_view bytes() const noexcept {
        return bytes_;
    }
    [[nodiscard]] http2_received_data_credit take_credit() & noexcept {
        return std::move(credit_);
    }

private:
    friend class http2_event;
    http2_tunnel_data_event(detail::http2_connection_owner_endpoint* endpoint, std::uint32_t stream_id,
        std::string_view bytes_value, std::uint32_t credit) noexcept
        : stream_id_(stream_id),
          bytes_(bytes_value),
          credit_(endpoint, stream_id, credit) {}
    std::uint32_t stream_id_;
    std::string_view bytes_;
    http2_received_data_credit credit_;
};

class http2_tunnel_end_event final {
public:
    [[nodiscard]] std::uint32_t stream_id() const noexcept {
        return stream_id_;
    }

private:
    friend class detail::http2_connection;
    explicit constexpr http2_tunnel_end_event(std::uint32_t stream_id) noexcept
        : stream_id_(stream_id) {}
    std::uint32_t stream_id_;
};

class http2_stream_closed_event final {
public:
    [[nodiscard]] std::uint32_t stream_id() const noexcept {
        return stream_id_;
    }
    [[nodiscard]] http2_stream_close_source source() const noexcept {
        return source_;
    }
    [[nodiscard]] http2_error_code error() const noexcept {
        return error_;
    }

private:
    friend class detail::http2_connection;
    constexpr http2_stream_closed_event(
        std::uint32_t stream_id, http2_stream_close_source source_value, http2_error_code error) noexcept
        : stream_id_(stream_id),
          source_(source_value),
          error_(error) {}
    std::uint32_t stream_id_;
    http2_stream_close_source source_;
    http2_error_code error_;
};

// Client only: the stream this endpoint opened was closed before the server
// processed it, so the request may be retried (RFC 9113 section 8.7). Published
// for streams above a peer GOAWAY's last stream id (after the GOAWAY event) and
// for RST_STREAM(REFUSED_STREAM) received before the final response head; the
// latter leaves the connection usable. No stream_closed event follows.
class http2_request_unprocessed_event final {
public:
    [[nodiscard]] std::uint32_t stream_id() const noexcept {
        return stream_id_;
    }

private:
    friend class detail::http2_connection;
    explicit constexpr http2_request_unprocessed_event(std::uint32_t stream_id) noexcept
        : stream_id_(stream_id) {}
    std::uint32_t stream_id_;
};

class http2_goaway_event final {
public:
    [[nodiscard]] std::uint32_t last_stream_id() const noexcept {
        return last_stream_id_;
    }
    [[nodiscard]] http2_error_code error() const noexcept {
        return error_;
    }

private:
    friend class detail::http2_connection;
    constexpr http2_goaway_event(std::uint32_t last_stream_id, http2_error_code error) noexcept
        : last_stream_id_(last_stream_id),
          error_(error) {}
    std::uint32_t last_stream_id_;
    http2_error_code error_;
};

class http2_event final {
public:
    http2_event(const http2_event&) = delete;
    http2_event& operator=(const http2_event&) = delete;
    http2_event(http2_event&&) noexcept = default;
    http2_event& operator=(http2_event&&) = delete;
    [[nodiscard]] const http2_informational_head_event* informational_head() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http2_informational_head_event>(&value_);
    }
    const http2_informational_head_event* informational_head() const&& = delete;
    [[nodiscard]] http2_request_head_event* request_head() & noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http2_request_head_event>(&value_);
    }
    [[nodiscard]] const http2_request_head_event* request_head() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http2_request_head_event>(&value_);
    }
    const http2_request_head_event* request_head() const&& = delete;
    [[nodiscard]] const http2_response_head_event* response_head() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http2_response_head_event>(&value_);
    }
    [[nodiscard]] http2_response_head_event* response_head() & noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http2_response_head_event>(&value_);
    }
    const http2_response_head_event* response_head() const&& = delete;
    [[nodiscard]] http2_message_body_chunk_event* message_body_chunk() & noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http2_message_body_chunk_event>(&value_);
    }
    [[nodiscard]] const http2_message_body_chunk_event* message_body_chunk() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http2_message_body_chunk_event>(&value_);
    }
    const http2_message_body_chunk_event* message_body_chunk() const&& = delete;
    [[nodiscard]] const http2_message_end_event* message_end() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http2_message_end_event>(&value_);
    }
    [[nodiscard]] http2_message_end_event* message_end() & noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http2_message_end_event>(&value_);
    }
    const http2_message_end_event* message_end() const&& = delete;
    [[nodiscard]] http2_tunnel_data_event* tunnel_data() & noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http2_tunnel_data_event>(&value_);
    }
    [[nodiscard]] const http2_tunnel_data_event* tunnel_data() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http2_tunnel_data_event>(&value_);
    }
    const http2_tunnel_data_event* tunnel_data() const&& = delete;
    [[nodiscard]] const http2_tunnel_end_event* tunnel_end() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http2_tunnel_end_event>(&value_);
    }
    const http2_tunnel_end_event* tunnel_end() const&& = delete;
    [[nodiscard]] const http2_stream_closed_event* stream_closed() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http2_stream_closed_event>(&value_);
    }
    const http2_stream_closed_event* stream_closed() const&& = delete;
    [[nodiscard]] const http2_request_unprocessed_event* request_unprocessed() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http2_request_unprocessed_event>(&value_);
    }
    const http2_request_unprocessed_event* request_unprocessed() const&& = delete;
    [[nodiscard]] const http2_goaway_event* goaway() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http2_goaway_event>(&value_);
    }
    const http2_goaway_event* goaway() const&& = delete;

    [[nodiscard]] http_origin_advertisement* origin_advertisement() & noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http_origin_advertisement>(&value_);
    }
    [[nodiscard]] const http_origin_advertisement* origin_advertisement() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http_origin_advertisement>(&value_);
    }
    const http_origin_advertisement* origin_advertisement() const&& = delete;
    [[nodiscard]] http_alternative_service_advertisement* alternative_service_advertisement() & noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http_alternative_service_advertisement>(&value_);
    }
    [[nodiscard]] const http_alternative_service_advertisement* alternative_service_advertisement() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http_alternative_service_advertisement>(&value_);
    }
    const http_alternative_service_advertisement* alternative_service_advertisement() const&& = delete;
    [[nodiscard]] const http_priority_update* priority_update() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http_priority_update>(&value_);
    }
    const http_priority_update* priority_update() const&& = delete;

    // Peer frame metadata, produced only with
    // http2_connection_options::receive_frame_metadata_. Each committed frame
    // yields at most one such event, queued in wire order with the others; a
    // frame rejected with a connection error yields none.
    // - received_settings: each applied non-ACK SETTINGS (an ACK yields none).
    // - received_window_update: each applied WINDOW_UPDATE; an overflowing
    //   increment, or one on a closed or unknown stream, yields none. A zero
    //   increment, on any stream, is a PROTOCOL_ERROR connection error.
    // - received_priority: the priority fields of an accepted HEADERS frame,
    //   ahead of every event that frame produces (such as its request head).
    //   A PRIORITY frame yields none.
    [[nodiscard]] const http2_received_settings* received_settings() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http2_received_settings>(&value_);
    }
    const http2_received_settings* received_settings() const&& = delete;
    [[nodiscard]] const http2_received_window_update* received_window_update() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http2_received_window_update>(&value_);
    }
    const http2_received_window_update* received_window_update() const&& = delete;
    [[nodiscard]] const http2_received_priority* received_priority() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http2_received_priority>(&value_);
    }
    const http2_received_priority* received_priority() const&& = delete;

    [[nodiscard]] const http2_push_promise_event* push_promise() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http2_push_promise_event>(&value_);
    }
    const http2_push_promise_event* push_promise() const&& = delete;

private:
    friend class http2_connection;
    friend class detail::http2_connection_owner_endpoint;
    using value_type = std::variant<http2_informational_head_event, http2_request_head_event,
        http2_response_head_event, http2_message_body_chunk_event, http2_message_end_event,
        http2_tunnel_data_event, http2_tunnel_end_event, http2_stream_closed_event,
        http2_request_unprocessed_event, http2_goaway_event, http2_push_promise_event, http_priority_update,
        http_origin_advertisement, http_alternative_service_advertisement, http2_received_settings,
        http2_received_window_update, http2_received_priority>;
    template <typename event_type>
    explicit http2_event(event_type event) noexcept
        : value_(std::move(event)) {}
    [[nodiscard]] static http2_event request_head(detail::http2_connection_owner_endpoint* endpoint,
        std::uint32_t id, http_request_expectations expectations,
        http_request_content_indication content, http2_server_request_snapshot snapshot) noexcept {
        return http2_event(http2_request_head_event(
            endpoint, id, expectations, content, snapshot));
    }
    [[nodiscard]] static http2_event response_head(std::uint32_t id, http_client_response_head head,
        std::optional<http_client_request_content_signal> signal, bool stream_ended) noexcept {
        return http2_event(http2_response_head_event(id, std::move(head), signal, stream_ended));
    }
    [[nodiscard]] static http2_event message_body_chunk(detail::http2_connection_owner_endpoint* endpoint,
        std::uint32_t id, std::string_view bytes_value, std::uint32_t credit) noexcept {
        return http2_event(http2_message_body_chunk_event(endpoint, id, bytes_value, credit));
    }
    [[nodiscard]] static http2_event message_end(std::uint32_t id,
        std::vector<http_header> trailers,
        http2_message_content_semantics content_semantics) noexcept {
        return http2_event(
            http2_message_end_event(id, std::move(trailers), content_semantics));
    }
    [[nodiscard]] static http2_event tunnel_data(detail::http2_connection_owner_endpoint* endpoint,
        std::uint32_t id, std::string_view bytes_value, std::uint32_t credit) noexcept {
        return http2_event(http2_tunnel_data_event(endpoint, id, bytes_value, credit));
    }
    value_type value_;
};

}  // namespace ruvia

namespace ruvia {

enum class http2_server_request_release_status : std::uint8_t { released,
    closed };
struct http2_connection_options final {
    // Client-only: http2_connection::server throws std::invalid_argument when set.
    bool enable_push_{false};
    // Client-only, like enable_push_.
    // Enable only for an authenticated h2 connection to an origin server.
    // RFC 8336 forbids using ORIGIN over h2c or through an explicit proxy.
    bool receive_origin_advertisements_{false};
    // Deliver the peer's frame metadata as received_settings,
    // received_window_update and received_priority (HEADERS priority fields
    // only) events, interleaved with the other events in wire order (see
    // http2_event). Off, none is produced and no metadata work is done.
    // Request pseudo-header order is always in the request-head event's
    // request_view().
    bool receive_frame_metadata_{false};
    // Independent synchronous, allocation-free wire/HPACK observations. These
    // include raw facts the normalized received_* events intentionally omit.
    http2_frame_observer_type frame_observer_{nullptr};
    http2_header_observer_type header_observer_{nullptr};
    void* observer_context_{nullptr};
};

struct http2_regular_request_head_view final {
    borrowed_text method_{"GET"};
    borrowed_text scheme_{"https"};
    std::optional<borrowed_text> authority_{};
    borrowed_text target_{"/"};
    borrowed_http_headers headers_{};
    http_request_content_plan content_{http_request_content_plan::none()};
    http_client_request_expectation expectation_{http_client_request_expectation::none};
    http_request_field_source field_source_{http_request_field_source::generated};
};

struct http2_connect_request_head_view final {
    borrowed_text authority_{};
    borrowed_http_headers headers_{};
    http_request_field_source field_source_{http_request_field_source::generated};
};

struct http2_extended_connect_request_head_view final {
    borrowed_text protocol_{};
    borrowed_text scheme_{"https"};
    borrowed_text authority_{};
    borrowed_text target_{"/"};
    borrowed_http_headers headers_{};
    http_request_field_source field_source_{http_request_field_source::generated};
};

// Stable HTTP/2 sans-I/O driver backed by one nghttp2 session per connection.
// Feed transport bytes, drain typed events, and write out take_output_batch().
// nghttp2 owns framing, HPACK, stream states, flow control and SETTINGS/PING/
// GOAWAY handling; this class owns HTTP message semantics, leases, credits and
// the abuse budgets. nghttp2 buffers an incomplete trailing frame itself.
// The storage behind an accepted feed() input must remain alive and unchanged
// until every event produced by that call has been taken and every event-derived
// byte view has expired. Calling feed() again may invalidate those views; owning
// string temporaries are rejected at compile time.
// The event queue capacity grows geometrically and is retained for reuse.
// Receive-field text growth reserves the whole name/value pair before copying.
// Request field capacity includes synthesized Host and coalesced Cookie. A head
// that cannot fit the public request view resets its stream before publication.
// Outbound HPACK never uses the dynamic table; credentials (authorization,
// proxy-authorization, cookie, set-cookie) are sent never-indexed.
// An allocation failure inside nghttp2 or while buffering its output, and an
// exception thrown while nghttp2 runs a callback, fail the connection (with
// GOAWAY(INTERNAL_ERROR) when it can still be queued) and propagate; nothing
// is rolled back for a retry.
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
// nghttp2 enforces the wire rules, so these error classes are its own:
// - A zero WINDOW_UPDATE increment on any stream, a HEADERS frame too short for
//   its padding or priority fields, a self-dependent HEADERS priority, a
//   SETTINGS_ENABLE_CONNECT_PROTOCOL value other than 0 or 1 (either role), and
//   a peer stream beyond our acknowledged SETTINGS_MAX_CONCURRENT_STREAMS are
//   PROTOCOL_ERROR connection errors. Before the peer acknowledges our
//   SETTINGS, a stream beyond the advertised limit is refused with
//   RST_STREAM(REFUSED_STREAM), and the peer may only use the RFC default
//   65535-byte stream window (RFC 9113 section 6.9.2).
// - A bad client preface is a PROTOCOL_ERROR; a SETTINGS frame with more than
//   32 entries or a header block with more than 1024 CONTINUATION frames ends
//   in GOAWAY(ENHANCE_YOUR_CALM).
// - Closed streams are not remembered: frames for a stream nghttp2 already
//   retired are ignored. A PUSH_PROMISE whose associated stream is gone has its
//   promised stream reset with CANCEL, and one arriving after our GOAWAY is
//   discarded. PRIORITY frames are read and dropped (RFC 9113 section 5.3.2
//   deprecates them).
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
    // A server ignores a received SETTINGS_ENABLE_CONNECT_PROTOCOL of 0 or 1 (RFC
    // 8441 section 3: receipt by a server "does not have any impact").
    [[nodiscard]] bool received_peer_settings() const noexcept;
    // Read-only observation for transport timeout decisions while a CONTINUATION
    // sequence is incomplete.
    [[nodiscard]] bool header_block_in_progress() const noexcept;
    [[nodiscard]] http2_feed_result feed(std::string_view input);
    template <detail::http_temporary_owning_char_string input_type>
    http2_feed_result feed(input_type&&) = delete;
    [[nodiscard]] std::optional<http2_event> next_event();
    // Appends whole frame boundaries (the first frame may exceed max_bytes).
    // A complete unconsumed batch swaps storage when output is empty;
    // all other cases copy the selected bytes.
    // On copy failure the output cursor and observer are untouched. The cursor
    // always rests on a frame boundary; an unbounded max_bytes drains every pending frame.
    // The cursor is committed before DATA observations. An observer may
    // submit or drain more output; those frames remain independent of this batch.
    // A client preface is treated as one indivisible segment.
    [[nodiscard]] http2_output_batch_result take_output_batch(std::size_t max_bytes,
        std::string& output, http2_data_output_observer_type observer = nullptr,
        void* observer_context = nullptr);
    [[nodiscard]] bool wants_write() const noexcept;

    // Point-in-time client request-stream capacity. This does not reserve a
    // stream; it clamps local_limit to the protocol core and peer SETTINGS
    // limits, and returns zero for a server or unavailable client connection.
    [[nodiscard]] std::size_t available_request_streams(std::size_t local_limit) const noexcept;

    // Submitted fields are encoded/owned before returning; a successful submission
    // is leased to the owner until its terminal event or submit_reset().
    [[nodiscard]] std::variant<std::uint32_t, http2_request_head_submit_error> submit_request_head(
        const http2_regular_request_head_view& request);
    [[nodiscard]] std::variant<std::uint32_t, http2_request_head_submit_error> submit_request_head(
        const http2_connect_request_head_view& request);
    [[nodiscard]] std::variant<std::uint32_t, http2_request_head_submit_error> submit_request_head(
        const http2_extended_connect_request_head_view& request);
    // Serializes at most min(bytes.size(), max_payload_bytes, available send window).
    // Only the returned consumed_bytes_ advance Content-Length; no input view or
    // unconsumed suffix is retained. Retry the suffix after obtaining output credit
    // and/or a WINDOW_UPDATE/SETTINGS change. complete means this input was consumed,
    // not that its frames reached the socket. Frame headers are additional overhead.
    // Validate the complete input's length/terminal intent before consuming
    // anything: input beyond the declared Content-Length, or a terminal input
    // short of it, returns {0, invalid_message}. A stream whose content is not
    // open throws std::logic_error.
    // END_STREAM is emitted only after consuming this entire input. An empty
    // terminal input is still a DATA frame. The nghttp2 backend defers it until
    // both connection and stream send windows are positive; otherwise
    // it returns {0, backpressured} without changing anything, and the caller
    // retries once send_window_state() reports available window.
    [[nodiscard]] http2_data_submit_result submit_data(std::uint32_t stream_id,
        std::string_view bytes, http2_end_stream end_stream, std::size_t max_payload_bytes);
    // Finish only after all source bytes were consumed. Trailer fields are encoded
    // before returning. Without trailers the END_STREAM rides an empty DATA frame
    // and returns backpressured, changing nothing, under the same window rule as
    // submit_data(). Content short of the declared Content-Length, or a trailer
    // field that is invalid, not permitted in request trailers (RFC 9110
    // section 6.5.1) or over the header list limit, returns invalid_message and
    // changes nothing; a stream whose request content is not open throws
    // std::logic_error.
    [[nodiscard]] http2_finish_status finish_request(std::uint32_t stream_id,
        std::span<const http_header_view> trailers = {});
    [[nodiscard]] http2_request_content_release_status release_request_content(
        std::uint32_t stream_id) noexcept;
    // Returns stream_limit without sending PUSH_PROMISE when the open pushed streams
    // already reach the peer's SETTINGS_MAX_CONCURRENT_STREAMS (RFC 9113 section
    // 8.4), including when it is 0. Reserved promises do not count (section 5.1.2);
    // a response head that later finds no slot gets peer_stream_limit_reached.
    // Commits a promise and returns the server request/storage lease used to
    // produce its response. Release or abandon it exactly like an incoming request.
    [[nodiscard]] std::variant<http2_request_head_event, http2_push_submit_error> submit_push_request(
        std::uint32_t associated_stream_id, http_push_request_view request);

    [[nodiscard]] http2_submit_status submit_interim_response_head(
        std::uint32_t stream_id, const http_interim_response_head& response);
    // Server CONNECT: DATA received before the accept/reject decision (RFC 9113
    // section 8.5; optimistic per RFC 9298 section 5 as updated by RFC 9931) is
    // published as tunnel_data events whose credit the owner holds until it consumes
    // the bytes after accepting; a 2xx accept (via submit_connect_response_head
    // or submit_websocket_handshake) publishes tunnel_end only after them.
    // After a non-2xx response, further DATA is discarded and its credit returned.
    // RFC 8441 :protocol websocket is handled the same way (RFC 6455 section 4.1
    // binds only the client). Unknown extension frame types on an established tunnel
    // are discarded (RFC 9113 section 5.5); ALTSVC and ORIGIN are ignored there too
    // (RFC 7838 section 4, RFC 8336 section 2.1).
    [[nodiscard]] http2_submit_status submit_connect_response_head(
        std::uint32_t stream_id, const http_response& response);
    [[nodiscard]] std::variant<websocket_extended_connect_negotiation, http2_websocket_handshake_submit_error>
    submit_websocket_handshake(std::uint32_t stream_id, const http_request& request,
        const websocket_handshake_validation_result& validation,
        websocket_server_handshake_options options = {});
    // Submit only the final buffered-response head. The committed plan tells the
    // caller whether/how much representation remains for DATA or file output.
    // Both final-head submissions reject connection-specific fields as
    // invalid_message (RFC 9113 section 8.2.2) except a Transfer-Encoding of
    // exactly `chunked` (OWS and ASCII case aside): that is the framing hint
    // HTTP/1 also replaces, and the HTTP/2 writer omits it.
    // Repeated Priority fields form one Structured Field dictionary. Valid
    // response parameters override the corresponding request scheduling values.
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
    // Call after all body bytes have been consumed. Encodes trailers before returning;
    // field storage may then be released. Incomplete Content-Length returns
    // invalid_message. Without trailers it returns backpressured under the same
    // window rule as finish_request().
    [[nodiscard]] http2_finish_status finish_response(
        std::uint32_t stream_id, const http_response_trailer_section& trailers);
    // ORIGIN and ALTSVC payloads are limited to the smaller of the peer's
    // SETTINGS_MAX_FRAME_SIZE and 16384 bytes (nghttp2's extension frame buffer);
    // larger ones return invalid_message.
    [[nodiscard]] http2_submit_status submit_origin_advertisement(std::span<const std::string_view> origins);
    [[nodiscard]] http2_submit_status submit_alternative_service_advertisement(std::uint32_t stream_id,
        std::string_view origin, std::string_view field_value);
    // Client only. Returns peer_capability_unavailable unless the server sent
    // SETTINGS_NO_RFC7540_PRIORITIES=1 (RFC 9218 section 2.1).
    [[nodiscard]] http2_submit_status submit_priority_update(std::uint32_t stream_id, http_priority_fields fields);

    // Resetting a stream does not return connection send credit or reopen other
    // streams' send windows.
    // Resetting an associated request cancels any PUSH_PROMISE block still in
    // progress on it: the promised stream is reset with CANCEL and publishes no event.
    // A stream nghttp2 no longer tracks (it keeps no closed-stream history,
    // RFC 9113 section 5.1) returns closed. Once both END_STREAM flags closed a
    // stream, only a PROTOCOL_ERROR reset for a malformed message is accepted;
    // it retires the stream locally and sends no frame.
    [[nodiscard]] http2_submit_status submit_reset(std::uint32_t stream_id, http2_error_code error);
    // Queues one PING (non-ACK) carrying an opaque payload distinct from the
    // graceful-drain PING. Liveness is any received frame (the peer's ACK
    // included); the ACK raises no event. While one is unacknowledged, further
    // calls return accepted without queueing another frame. Returns closed after
    // a connection error.
    [[nodiscard]] http2_submit_status submit_ping();
    // Queues SETTINGS_MAX_CONCURRENT_STREAMS through the protocol session so
    // its acknowledgement and admission limits stay synchronized. The limit
    // may tighten or restore the fixed local capacity (128); a larger value
    // returns invalid_message, and a failed connection returns closed.
    [[nodiscard]] http2_submit_status submit_max_concurrent_streams(std::uint32_t limit);
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
    // Consume the server request event only after every request-derived view has
    // expired. An event cannot be released through another connection.
    [[nodiscard]] http2_server_request_release_status release(http2_request_head_event&& request);
    // Graceful drain (RFC 9113 section 6.8). A server first sends GOAWAY(2^31-1,
    // NO_ERROR) and a PING, and keeps admitting peer streams until the PING ACK or
    // finish_drain(); then it sends GOAWAY(NO_ERROR) with the last peer stream id.
    // Streams above that boundary are discarded without a reply (the peer learns of
    // them from the GOAWAY). A peer stream that exceeds the round-trip bound (128
    // further streams before the ACK, a product bound) gets RST_STREAM(REFUSED_STREAM)
    // and the final GOAWAY follows at once. The ACK itself is not required: the
    // owner bounds the wait in time with finish_drain(). A client sends its final
    // GOAWAY immediately. Idempotent.
    void begin_drain();
    // Makes the drain final now: sends GOAWAY(NO_ERROR) with the last peer stream id
    // and refuses later peer streams, entering the drain without the round trip when
    // none is in progress. For a server whose round-trip wait reached the owner's
    // time bound, or whose peer can open no further streams (its input ended). A
    // valid peer GOAWAY does this itself: its sender opens no further streams.
    // No-op once the drain is final or after a connection error.
    void finish_drain();
    // Immediate shutdown (RFC 9113 section 6.8): queues GOAWAY(error) naming
    // the last peer stream processed, discards every later inbound frame and
    // makes connection_error() report `error`, even no_error. Streams still
    // open get no further protocol progress; the owner closes the transport
    // after flushing the output. Ends a graceful drain too. No-op after a
    // connection error, whose code is kept.
    void terminate(http2_error_code error);
    // True from begin_drain(), finish_drain() or a valid peer GOAWAY on.
    [[nodiscard]] bool draining() const noexcept;
    // True once the final GOAWAY is queued; no later peer stream will be admitted,
    // so the owner may close after the remaining streams finish.
    [[nodiscard]] bool drain_final() const noexcept;
    [[nodiscard]] std::optional<http2_error_code> connection_error() const noexcept;

private:
    friend websocket_handshake_validation_result validate_http2_websocket_handshake(
        http2_connection&, std::uint32_t, const http_request&,
        websocket_handshake_validation_options) noexcept;
    http2_connection(http2_role role, const http2_connection_options& options);
    struct impl_deleter_type final {
        void operator()(detail::http2_connection_owner_endpoint* value) const noexcept;
    };
    std::unique_ptr<detail::http2_connection_owner_endpoint, impl_deleter_type> impl_;
};
// Validates an RFC 8441 extended-CONNECT handshake against the decoded stream
// state and its semantic request. The request and connection must remain alive
// for the duration of the call.
[[nodiscard]] websocket_handshake_validation_result validate_http2_websocket_handshake(
    http2_connection& connection, std::uint32_t stream_id,
    const http_request& request,
    websocket_handshake_validation_options options = {}) noexcept;

// RFC 8441 Extended CONNECT with :protocol websocket: the negotiation's
// Sec-WebSocket-* request fields followed by the application headers.
// Validate the final head with validate_extended_connect_websocket_client_response().
[[nodiscard]] std::variant<std::uint32_t, http2_request_head_submit_error> submit_http2_websocket_client_request(
    http2_connection& connection, const websocket_client_negotiation& negotiation, std::string_view scheme,
    std::string_view authority, std::string_view target, borrowed_http_headers headers = {},
    http_request_field_source source = http_request_field_source::generated);

}  // namespace ruvia
