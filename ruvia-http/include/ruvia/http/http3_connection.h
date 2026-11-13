#pragma once

#include <ngtcp2/ngtcp2.h>

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/http_client.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_request_target.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_response_server.h"
#include "ruvia/http/http_status.h"
#include "ruvia/http/websocket_handshake.h"

namespace ruvia {

class http3_connection;

// A validated HTTP/3 request head (RFC 9114 §4.3.1), published by a server
// http3_connection as http3_request_head_event. nghttp3 rejects the
// framing-level violations of §4.1.2 and §4.3.1 -- uppercase names,
// misplaced, repeated or missing pseudo-header fields, connection-specific
// fields, TE other than "trailers", malformed Content-Length -- and the
// connection adds the request semantics every HTTP version shares in
// ruvia-http (target grammar, :authority/Host agreement, the recipient field
// rules, Expect, Cookie joining). A head it cannot represent as an
// http_request is a malformed request the connection answers itself with
// H3_MESSAGE_ERROR (§4.1.2); no head is published for it.
class http3_request_head final {
public:
    http3_request_head(http3_request_head&&) noexcept = default;
    http3_request_head& operator=(http3_request_head&&) = delete;
    http3_request_head(const http3_request_head&) = delete;
    http3_request_head& operator=(const http3_request_head&) = delete;

    [[nodiscard]] std::int64_t stream_id() const noexcept {
        return stream_id_;
    }
    [[nodiscard]] std::string_view method() const& noexcept RUVIA_LIFETIMEBOUND;
    std::string_view method() const&& = delete;
    [[nodiscard]] std::string_view scheme() const& noexcept RUVIA_LIFETIMEBOUND;
    std::string_view scheme() const&& = delete;
    [[nodiscard]] std::string_view authority() const& noexcept RUVIA_LIFETIMEBOUND;
    std::string_view authority() const&& = delete;
    [[nodiscard]] std::string_view path() const& noexcept RUVIA_LIFETIMEBOUND;
    std::string_view path() const&& = delete;
    [[nodiscard]] std::string_view protocol() const& noexcept RUVIA_LIFETIMEBOUND;
    std::string_view protocol() const&& = delete;
    // CONNECT, with or without :protocol (RFC 9114 §4.4, RFC 9220).
    [[nodiscard]] bool connect() const noexcept;
    // The HEADERS frame ended the request stream: no content follows.
    [[nodiscard]] bool stream_ended() const noexcept {
        return stream_ended_;
    }
    [[nodiscard]] bool early_data() const noexcept {
        return early_data_;
    }
    [[nodiscard]] http_request_content_indication content() const noexcept {
        return stream_ended_ || declared_content_length_ == std::uint64_t{0}
                   ? http_request_content_indication::no_content
                   : http_request_content_indication::will_follow;
    }
    [[nodiscard]] std::optional<std::uint64_t> declared_content_length() const noexcept {
        return declared_content_length_;
    }
    [[nodiscard]] http_server_expectation_plan expectation_plan(
        http_unsupported_expectation_policy policy) const noexcept {
        return expectations_.server_plan(content(), policy);
    }

    // Views in the request borrow this head; it must outlive the request.
    [[nodiscard]] http_request build_request(
        borrowed_text body = {}) const& RUVIA_LIFETIMEBOUND;
    http_request build_request(
        borrowed_text body = {}) const&& = delete;

private:
    friend class http3_connection;
    struct regular_field final {
        std::uint32_t index_;
        detail::http_field_kind kind_;
    };

    http3_request_head(std::int64_t stream_id, std::vector<http_header> fields, bool stream_ended,
        bool early_data);
    // nullopt: the head is malformed (RFC 9114 §4.1.2).
    [[nodiscard]] static std::optional<http3_request_head> try_create(std::int64_t stream_id,
        std::vector<http_header> fields, bool stream_ended, bool early_data);
    [[nodiscard]] bool validate();
    [[nodiscard]] std::string_view field_value(std::uint32_t index) const noexcept;

    static constexpr std::uint32_t absent = 0xffffffffU;

    std::vector<http_header> fields_;
    std::vector<regular_field> regular_;
    std::string cookie_;
    std::int64_t stream_id_{-1};
    std::uint32_t method_{absent};
    std::uint32_t scheme_{absent};
    std::uint32_t authority_{absent};
    std::uint32_t path_{absent};
    std::uint32_t protocol_{absent};
    std::uint32_t host_{absent};
    std::optional<std::uint64_t> declared_content_length_;
    std::uint32_t first_cookie_{absent};
    http_request_expectations expectations_{};
    bool stream_ended_{false};
    bool early_data_{false};
};

// RFC 9220 uses the extended CONNECT request in place of the HTTP/1 key exchange.
[[nodiscard]] websocket_handshake_validation_result validate_http3_websocket_handshake(
    const http3_request_head& head, const http_request& request,
    websocket_handshake_validation_options options = {}) noexcept;

}  // namespace ruvia

namespace ruvia {
class http3_connection;
class http3_quic_connection;
namespace detail {
class http3_received_data_ledger;
}  // namespace detail

enum class http3_role : std::uint8_t { client,
    server };

// HTTP/3 error codes (RFC 9114 §8.1), the QPACK codes (RFC 9204 §6) and
// H3_DATAGRAM_ERROR (RFC 9297 §5.2), carried by RESET_STREAM, STOP_SENDING
// and CONNECTION_CLOSE. A received code outside this list, including the
// reserved 0x1f * N + 0x21 values, keeps its exact value; RFC 9114 §9 makes
// an unknown code equivalent to H3_NO_ERROR.
enum class http3_error_code : std::uint64_t {
    no_error = 0x0100,
    general_protocol_error = 0x0101,
    internal_error = 0x0102,
    stream_creation_error = 0x0103,
    closed_critical_stream = 0x0104,
    frame_unexpected = 0x0105,
    frame_error = 0x0106,
    excessive_load = 0x0107,
    id_error = 0x0108,
    settings_error = 0x0109,
    missing_settings = 0x010a,
    request_rejected = 0x010b,
    request_cancelled = 0x010c,
    request_incomplete = 0x010d,
    message_error = 0x010e,
    connect_error = 0x010f,
    version_fallback = 0x0110,
    qpack_decompression_failed = 0x0200,
    qpack_encoder_stream_error = 0x0201,
    qpack_decoder_stream_error = 0x0202,
    datagram_error = 0x33,
};

// HTTP/3 frame types (RFC 9114 §7.2, RFC 9412 §2, RFC 9218 §7.2). A frame
// type outside this list, including the reserved ones of §7.2.8, keeps its
// exact value.
enum class http3_frame_type : std::uint64_t {
    data = 0x0,
    headers = 0x1,
    cancel_push = 0x3,
    settings = 0x4,
    push_promise = 0x5,
    goaway = 0x7,
    origin = 0xc,
    max_push_id = 0xd,
    priority_update_request = 0xf0700,
    priority_update_push = 0xf0701,
};

// Codec input and output driven by the QUIC owner: read_stream(),
// read_datagram(), peek_output() and the stream bookkeeping calls.
// would_block: no output is ready. events_pending: the input waits until
// next_event() has drained the previous input's events. closed: the
// connection already failed, or the stream is gone. invalid_state: the call
// does not fit the role, the stream or the bound control streams.
// protocol_error: this call failed the connection; connection_error() then
// carries the code for CONNECTION_CLOSE.
enum class http3_transport_status : std::uint8_t {
    accepted,
    would_block,
    events_pending,
    closed,
    invalid_state,
    protocol_error,
};

struct http3_transport_result final {
    http3_transport_status status_{http3_transport_status::accepted};
    // Set with protocol_error, and with closed after a connection error: the
    // HTTP/3 code for the QUIC CONNECTION_CLOSE.
    std::optional<http3_error_code> error_code_{};
    // read_stream(): framing and QPACK bytes the codec consumed itself, which
    // the QUIC owner returns at once as receive flow credit (RFC 9000 §4.1).
    // DATA payload returns through http3_received_data_credit instead.
    std::size_t credit_bytes_{0};
};

// Head and control submissions. closed is an expected race with a reset
// stream or a failed connection; invalid_state is a request the codec's role
// or stream state cannot take. invalid_message is a message HTTP/3 cannot
// carry (RFC 9114 §4.2, §4.3) or that exceeds a field section limit,
// returned so a caller forwarding peer data can answer it.
// peer_settings_pending: the operation depends on peer SETTINGS that have
// not arrived (RFC 9114 §7.2.4); retry after peer_settings() reports them.
// peer_capability_unavailable: the peer's SETTINGS do not enable the feature.
// backpressured: submit_origin_advertisement() found the control stream's
// output budget full; retry after acknowledgements free it.
enum class http3_submit_status : std::uint8_t {
    accepted,
    closed,
    invalid_state,
    invalid_message,
    peer_settings_pending,
    peer_capability_unavailable,
    backpressured,
};

// complete: the whole input was copied. backpressured: only consumed_bytes_
// (possibly none) fit the stream's output budget; the caller keeps the rest
// and retries after acknowledgements free budget (pending_output_bytes()).
// closed: the stream is gone or its send half was abandoned.
// invalid_message: the input would exceed the declared Content-Length;
// nothing was consumed.
enum class http3_data_submit_status : std::uint8_t {
    complete,
    backpressured,
    closed,
    invalid_message,
};

// The codec copies exactly the consumed prefix; no input view is retained.
struct http3_data_submit_result final {
    std::size_t consumed_bytes_{0};
    http3_data_submit_status status_{http3_data_submit_status::complete};
};

enum class http3_finish_status : std::uint8_t {
    accepted,
    // The stream disappeared or its send half was abandoned.
    closed,
    // The submitted DATA does not reach the declared Content-Length, or the
    // trailer section cannot be sent. Nothing changed; the stream stays open.
    invalid_message,
};

struct http3_regular_request_head_view final {
    borrowed_text method_{"GET"};
    borrowed_text scheme_{"https"};
    std::optional<borrowed_text> authority_{};
    borrowed_text target_{"/"};
    borrowed_http_headers headers_{};
    http_request_content_plan content_{http_request_content_plan::none()};
    http_client_request_expectation expectation_{http_client_request_expectation::none};
    http_request_field_source field_source_{http_request_field_source::generated};
};

struct http3_connect_request_head_view final {
    borrowed_text authority_{};
    borrowed_http_headers headers_{};
    http_request_field_source field_source_{http_request_field_source::generated};
};

struct http3_extended_connect_request_head_view final {
    borrowed_text protocol_{};
    borrowed_text scheme_{"https"};
    borrowed_text authority_{};
    borrowed_text target_{"/"};
    borrowed_http_headers headers_{};
    http_request_field_source field_source_{http_request_field_source::generated};
};

// Receive flow-control credit (RFC 9000 §4.1) for the DATA payload of one
// event. A credit is linear and has a single way back: destroying a
// still-valid token returns its exact byte count to the connection's credit
// sink without throwing, so a consumer holds it until the bytes are
// consumed. A credit that outlives its connection returns nothing.
class http3_received_data_credit final {
public:
    ~http3_received_data_credit();
    http3_received_data_credit(const http3_received_data_credit&) = delete;
    http3_received_data_credit& operator=(const http3_received_data_credit&) = delete;
    http3_received_data_credit(http3_received_data_credit&& other) noexcept;
    http3_received_data_credit& operator=(http3_received_data_credit&&) = delete;
    // Combines credits of one stream without returning any bytes. Success
    // consumes other; invalid, mismatched or overflowing tokens throw.
    void merge(http3_received_data_credit&& other);
    [[nodiscard]] bool valid() const noexcept {
        return ledger_ != nullptr && bytes_ != 0;
    }

private:
    friend class http3_connection;
    friend class http3_data_event;
    http3_received_data_credit(detail::http3_received_data_ledger* ledger, std::int64_t stream_id,
        std::size_t bytes) noexcept;
    void swap(http3_received_data_credit& other) noexcept;
    detail::http3_received_data_ledger* ledger_{nullptr};
    std::int64_t stream_id_{-1};
    std::size_t bytes_{0};
};

// Server: a request head that passed validation (see http3_request_head).
class http3_request_head_event final {
public:
    http3_request_head_event(http3_request_head_event&&) noexcept = default;
    http3_request_head_event& operator=(http3_request_head_event&&) = delete;
    [[nodiscard]] std::int64_t stream_id() const noexcept {
        return head_.stream_id();
    }
    [[nodiscard]] const http3_request_head& head() const& noexcept RUVIA_LIFETIMEBOUND {
        return head_;
    }
    const http3_request_head& head() const&& = delete;
    [[nodiscard]] http3_request_head take_head() && noexcept {
        return std::move(head_);
    }

private:
    friend class http3_connection;
    explicit http3_request_head_event(http3_request_head head) noexcept
        : head_(std::move(head)) {}
    http3_request_head head_;
};

// DATA payload of a request stream. With own_data_events_ the event owns its
// bytes; otherwise they borrow the read_stream() input. The credit stays with
// the event until take_credit() hands it to the consumer.
// Move assignment returns the target's untaken credit.
class http3_data_event final {
public:
    http3_data_event(const http3_data_event&) = delete;
    http3_data_event& operator=(const http3_data_event&) = delete;
    http3_data_event(http3_data_event&& other) noexcept;
    http3_data_event& operator=(http3_data_event&& other) noexcept;

    [[nodiscard]] std::int64_t stream_id() const noexcept {
        return stream_id_;
    }
    [[nodiscard]] std::string_view bytes() const& noexcept RUVIA_LIFETIMEBOUND {
        return bytes_;
    }
    std::string_view bytes() const&& = delete;
    [[nodiscard]] http3_received_data_credit take_credit() & noexcept {
        return std::move(credit_);
    }

private:
    friend class http3_connection;
    http3_data_event(std::int64_t stream_id, std::string_view bytes,
        bool own_bytes, http3_received_data_credit credit);
    std::int64_t stream_id_{-1};
    std::string_view bytes_;
    std::string owned_bytes_;
    bool owns_bytes_{false};
    http3_received_data_credit credit_;
};

// Client: a 1xx response head (RFC 9114 §4.1). request_content_signal() is
// continue_value for the first 100 (Continue) of a request submitted with
// Expect: 100-continue.
class http3_informational_head_event final {
public:
    http3_informational_head_event(http3_informational_head_event&&) noexcept = default;
    http3_informational_head_event& operator=(http3_informational_head_event&&) = delete;
    [[nodiscard]] std::int64_t stream_id() const noexcept {
        return stream_id_;
    }
    [[nodiscard]] const http_client_response_head& head() const& noexcept RUVIA_LIFETIMEBOUND {
        return head_;
    }
    const http_client_response_head& head() const&& = delete;
    [[nodiscard]] http_client_response_head take_head() && noexcept {
        return std::move(head_);
    }
    [[nodiscard]] std::optional<http_client_request_content_signal> request_content_signal() const noexcept {
        return signal_;
    }

private:
    friend class http3_connection;
    http3_informational_head_event(std::int64_t stream_id, http_client_response_head head,
        std::optional<http_client_request_content_signal> signal) noexcept
        : stream_id_(stream_id),
          head_(std::move(head)),
          signal_(signal) {}
    std::int64_t stream_id_;
    http_client_response_head head_;
    std::optional<http_client_request_content_signal> signal_;
};

// Client: the final response head. Fields are validated regular fields; the
// raw three-digit status is retained (received_status()) and status() applies
// RFC 9110 §15 semantics (600..999 process as 500). request_content_signal()
// is exchange_complete when the final head arrived while request content
// still waited for 100 (Continue). stream_ended(): the HEADERS frame ended
// the stream, so no content or tunnel DATA follows.
class http3_response_head_event final {
public:
    http3_response_head_event(http3_response_head_event&&) noexcept = default;
    http3_response_head_event& operator=(http3_response_head_event&&) = delete;
    [[nodiscard]] std::int64_t stream_id() const noexcept {
        return stream_id_;
    }
    [[nodiscard]] const http_client_response_head& head() const& noexcept RUVIA_LIFETIMEBOUND {
        return head_;
    }
    const http_client_response_head& head() const&& = delete;
    [[nodiscard]] http_client_response_head take_head() && noexcept {
        return std::move(head_);
    }
    [[nodiscard]] std::optional<http_client_request_content_signal> request_content_signal() const noexcept {
        return signal_;
    }
    [[nodiscard]] bool stream_ended() const noexcept {
        return stream_ended_;
    }

private:
    friend class http3_connection;
    http3_response_head_event(std::int64_t stream_id, http_client_response_head head,
        std::optional<http_client_request_content_signal> signal, bool stream_ended) noexcept
        : stream_id_(stream_id),
          head_(std::move(head)),
          signal_(signal),
          stream_ended_(stream_ended) {}
    std::int64_t stream_id_;
    http_client_response_head head_;
    std::optional<http_client_request_content_signal> signal_;
    bool stream_ended_;
};

// The peer ended its send half normally: the request or response content is
// complete, or a tunnel's receive direction ended. trailers() is the trailer
// section that preceded the end, empty when there was none. A client receives
// validated regular fields with those RFC 9110 §6.5.1 forbids in response
// trailers already discarded; a server receives the decoded regular fields
// (nghttp3 rejects pseudo-header fields in a trailer section, RFC 9114 §4.3)
// for http_request_trailers::append_received(). The span borrows the event:
// it is invalidated when the event is moved or destroyed, or once
// take_trailers() moved the fields out.
class http3_message_end_event final {
public:
    http3_message_end_event(http3_message_end_event&&) noexcept = default;
    http3_message_end_event& operator=(http3_message_end_event&&) = delete;
    [[nodiscard]] std::int64_t stream_id() const noexcept {
        return stream_id_;
    }
    [[nodiscard]] std::span<const http_header> trailers() const& noexcept RUVIA_LIFETIMEBOUND {
        return trailers_;
    }
    std::span<const http_header> trailers() const&& = delete;
    [[nodiscard]] std::vector<http_header> take_trailers() && noexcept {
        return std::move(trailers_);
    }

private:
    friend class http3_connection;
    http3_message_end_event(std::int64_t stream_id, std::vector<http_header> trailers) noexcept
        : stream_id_(stream_id),
          trailers_(std::move(trailers)) {}
    std::int64_t stream_id_;
    std::vector<http_header> trailers_;
};

enum class http3_transport_action : std::uint8_t {
    stop_sending,
    reset_stream,
};

enum class http3_transport_action_origin : std::uint8_t {
    // The codec asks the QUIC owner to send this frame; http3_quic_connection
    // does so before it publishes the event.
    local,
    // The peer's frame, already applied to the codec; never echo it.
    peer,
};

class http3_transport_action_event final {
public:
    [[nodiscard]] std::int64_t stream_id() const noexcept {
        return stream_id_;
    }
    [[nodiscard]] http3_transport_action action() const noexcept {
        return action_;
    }
    [[nodiscard]] http3_error_code error() const noexcept {
        return error_;
    }
    [[nodiscard]] http3_transport_action_origin origin() const noexcept {
        return origin_;
    }

private:
    friend class http3_connection;
    constexpr http3_transport_action_event(std::int64_t stream_id, http3_transport_action action,
        http3_error_code error, http3_transport_action_origin origin) noexcept
        : stream_id_(stream_id),
          action_(action),
          error_(error),
          origin_(origin) {}
    std::int64_t stream_id_;
    http3_transport_action action_;
    http3_error_code error_;
    http3_transport_action_origin origin_;
};

// Stream bytes nghttp3 consumed on its own after read_stream() returned:
// framing and QPACK input released once a blocked field section decoded, and
// DATA the codec discarded (nghttp3 deferred_consume semantics). No consumer
// ever holds them, so the QUIC owner returns them at once, exactly like
// http3_transport_result::credit_bytes_, rather than as a credit.
// http3_quic_connection applies them itself and never publishes this event.
class http3_deferred_consume_event final {
public:
    [[nodiscard]] std::int64_t stream_id() const noexcept {
        return stream_id_;
    }
    [[nodiscard]] std::size_t bytes() const noexcept {
        return bytes_;
    }

private:
    friend class http3_connection;
    constexpr http3_deferred_consume_event(std::int64_t stream_id, std::size_t bytes) noexcept
        : stream_id_(stream_id),
          bytes_(bytes) {}
    std::int64_t stream_id_;
    std::size_t bytes_;
};

// The stream is closed in both directions; no further event names it and its
// identifier may not be submitted to again. error() is present when the
// stream ended by RESET_STREAM or STOP_SENDING rather than by completing.
// A stream reported by http3_request_unprocessed_event publishes no close.
class http3_stream_close_event final {
public:
    [[nodiscard]] std::int64_t stream_id() const noexcept {
        return stream_id_;
    }
    [[nodiscard]] std::optional<http3_error_code> error() const noexcept {
        return error_;
    }

private:
    friend class http3_connection;
    constexpr http3_stream_close_event(std::int64_t stream_id, std::optional<http3_error_code> error) noexcept
        : stream_id_(stream_id),
          error_(error) {}
    std::int64_t stream_id_;
    std::optional<http3_error_code> error_;
};

// Client only: a request stream this endpoint opened that the server did not
// process, so the request may be retried on another connection (RFC 9114
// §4.1.1, §5.2). Published, after the GOAWAY event, for every open request
// stream at or above a GOAWAY's identifier, and for a stream the server reset
// or stopped with H3_REQUEST_REJECTED before its final response head. It is
// followed only by the local H3_REQUEST_CANCELLED transport actions for
// whichever direction of the stream is still open; no message, peer
// notification or stream close names the stream after it.
class http3_request_unprocessed_event final {
public:
    [[nodiscard]] std::int64_t stream_id() const noexcept {
        return stream_id_;
    }

private:
    friend class http3_connection;
    explicit constexpr http3_request_unprocessed_event(std::int64_t stream_id) noexcept
        : stream_id_(stream_id) {}
    std::int64_t stream_id_;
};

// The peer sent GOAWAY (RFC 9114 §5.2). For a client, id() is the first
// request stream the server will not process; for a server, the first push
// ID the client will not accept. nullopt: a shutdown notice whose identifier
// bounds no stream (2^62-4, or larger).
class http3_goaway_event final {
public:
    [[nodiscard]] std::optional<std::int64_t> id() const noexcept {
        return id_;
    }

private:
    friend class http3_connection;
    explicit constexpr http3_goaway_event(std::optional<std::int64_t> id) noexcept
        : id_(id) {}
    std::optional<std::int64_t> id_;
};

// An HTTP/3 Datagram (RFC 9297 §2.1) for a request stream whose head this
// codec decoded; payload() follows the Quarter Stream ID and borrows the
// event.
class http3_datagram_event final {
public:
    http3_datagram_event(http3_datagram_event&&) noexcept = default;
    http3_datagram_event& operator=(http3_datagram_event&&) = delete;
    [[nodiscard]] std::int64_t stream_id() const noexcept {
        return stream_id_;
    }
    [[nodiscard]] std::string_view payload() const& noexcept RUVIA_LIFETIMEBOUND {
        return payload_;
    }
    std::string_view payload() const&& = delete;
    [[nodiscard]] std::string take_payload() && noexcept {
        return std::move(payload_);
    }

private:
    friend class http3_connection;
    http3_datagram_event(std::int64_t stream_id, std::string payload) noexcept
        : stream_id_(stream_id),
          payload_(std::move(payload)) {}
    std::int64_t stream_id_;
    std::string payload_;
};

// One codec event. Exactly one accessor returns non-null.
class http3_event final {
public:
    http3_event(const http3_event&) = delete;
    http3_event& operator=(const http3_event&) = delete;
    http3_event(http3_event&&) noexcept = default;
    http3_event& operator=(http3_event&&) = delete;

    [[nodiscard]] http3_request_head_event* request_head() & noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http3_request_head_event>(&value_);
    }
    [[nodiscard]] const http3_request_head_event* request_head() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http3_request_head_event>(&value_);
    }
    const http3_request_head_event* request_head() const&& = delete;
    [[nodiscard]] http3_informational_head_event* informational_head() & noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http3_informational_head_event>(&value_);
    }
    [[nodiscard]] const http3_informational_head_event* informational_head() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http3_informational_head_event>(&value_);
    }
    const http3_informational_head_event* informational_head() const&& = delete;
    [[nodiscard]] http3_response_head_event* response_head() & noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http3_response_head_event>(&value_);
    }
    [[nodiscard]] const http3_response_head_event* response_head() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http3_response_head_event>(&value_);
    }
    const http3_response_head_event* response_head() const&& = delete;
    [[nodiscard]] http3_data_event* data() & noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http3_data_event>(&value_);
    }
    [[nodiscard]] const http3_data_event* data() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http3_data_event>(&value_);
    }
    const http3_data_event* data() const&& = delete;
    [[nodiscard]] http3_message_end_event* message_end() & noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http3_message_end_event>(&value_);
    }
    [[nodiscard]] const http3_message_end_event* message_end() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http3_message_end_event>(&value_);
    }
    const http3_message_end_event* message_end() const&& = delete;
    [[nodiscard]] const http3_transport_action_event* transport_action() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http3_transport_action_event>(&value_);
    }
    const http3_transport_action_event* transport_action() const&& = delete;
    [[nodiscard]] const http3_deferred_consume_event* deferred_consume() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http3_deferred_consume_event>(&value_);
    }
    const http3_deferred_consume_event* deferred_consume() const&& = delete;
    [[nodiscard]] const http3_stream_close_event* stream_close() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http3_stream_close_event>(&value_);
    }
    const http3_stream_close_event* stream_close() const&& = delete;
    [[nodiscard]] const http3_request_unprocessed_event* request_unprocessed() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http3_request_unprocessed_event>(&value_);
    }
    const http3_request_unprocessed_event* request_unprocessed() const&& = delete;
    [[nodiscard]] const http3_goaway_event* goaway() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http3_goaway_event>(&value_);
    }
    const http3_goaway_event* goaway() const&& = delete;
    [[nodiscard]] http3_datagram_event* datagram() & noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http3_datagram_event>(&value_);
    }
    [[nodiscard]] const http3_datagram_event* datagram() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http3_datagram_event>(&value_);
    }
    const http3_datagram_event* datagram() const&& = delete;
    [[nodiscard]] http_origin_advertisement* origin_advertisement() & noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http_origin_advertisement>(&value_);
    }
    [[nodiscard]] const http_origin_advertisement* origin_advertisement() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http_origin_advertisement>(&value_);
    }
    const http_origin_advertisement* origin_advertisement() const&& = delete;

private:
    friend class http3_connection;
    using value_type = std::variant<http3_request_head_event, http3_informational_head_event,
        http3_response_head_event, http3_data_event, http3_message_end_event, http3_transport_action_event,
        http3_deferred_consume_event, http3_stream_close_event, http3_request_unprocessed_event,
        http3_goaway_event, http3_datagram_event, http_origin_advertisement>;
    template <typename event_type>
    explicit http3_event(event_type event) noexcept
        : value_(std::move(event)) {}
    value_type value_;
};

// The SETTINGS the peer sent (RFC 9114 §7.2.4) that select optional features.
struct http3_peer_settings final {
    // SETTINGS_ENABLE_CONNECT_PROTOCOL (RFC 9220 §3).
    bool enable_connect_protocol_{false};
    // SETTINGS_H3_DATAGRAM (RFC 9297 §2.1.1).
    bool h3_datagram_{false};
    std::uint64_t max_field_section_size_{UINT64_MAX};
    std::uint64_t qpack_max_dtable_capacity_{0};
    std::uint64_t qpack_blocked_streams_{0};
};

// The Quarter Stream ID that prefixes every HTTP/3 Datagram of one request
// stream (RFC 9297 §2.1), as a QUIC variable-length integer.
struct http3_datagram_prefix final {
    [[nodiscard]] std::span<const std::byte> bytes() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::span<const std::byte>(bytes_.data(), size_);
    }
    std::span<const std::byte> bytes() const&& = delete;

    std::array<std::byte, 8> bytes_{};
    std::size_t size_{0};
};

struct http3_stream_output final {
    [[nodiscard]] std::size_t size() const noexcept;

    std::int64_t stream_id_{-1};
    std::array<std::span<const std::byte>, 16> buffers_{};
    std::size_t buffer_count_{0};
    bool fin_{false};
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

// A frame header the codec read on a request stream or on the peer's control
// stream (RFC 9114 §7.1), before the frame's payload is processed.
// observation_sequence_ counts the observed frame headers of the connection
// from zero, in the order read_stream() received them. Unknown and reserved
// types are observed with their exact value. Observation does not imply
// acceptance: validation and the mandatory error handling still run.
struct http3_frame_metadata final {
    std::uint64_t observation_sequence_{0};
    std::int64_t stream_id_{-1};
    http3_frame_type type_{http3_frame_type::data};
    std::uint64_t payload_length_{0};
};

enum class http3_field_section_kind : std::uint8_t { header,
    trailer };
enum class http3_field_observation_phase : std::uint8_t { begin,
    field,
    end };

// QPACK decoder facts for one received field section (RFC 9204), not
// validated request or response fields. begin precedes the first field;
// field follows QPACK decoding and precedes HTTP validation, including for a
// field that causes rejection; end means the section decoded completely, not
// that HTTP semantics accepted it. A blocked section (RFC 9204 §2.1.2) is
// observed once its dynamic table references resolve. name_/value_ borrow
// decoder storage only for this callback; both are empty on begin and end.
struct http3_field_metadata final {
    std::int64_t stream_id_{-1};
    http3_field_section_kind section_{http3_field_section_kind::header};
    std::size_t field_index_{0};
    http3_field_observation_phase phase_{http3_field_observation_phase::begin};
    std::string_view name_{};
    std::string_view value_{};
};

// Invoked synchronously inside read_stream(), never from next_event().
// Observers must not mutate, re-enter, move or destroy the connection until
// read_stream() returns. The context is borrowed for the lifetime of the
// connection. nullptr disables an observer with no extra work or allocation.
using http3_frame_observer_type = void (*)(void*, const http3_frame_metadata&) noexcept;
using http3_field_observer_type = void (*)(void*, const http3_field_metadata&) noexcept;

// Receives returned DATA credit: the application consumed `bytes` of
// `stream_id`'s payload, so the QUIC owner may extend the stream and
// connection receive windows by that amount (RFC 9000 §4.1). Called
// synchronously while a credit is destroyed; it must not throw or re-enter
// the codec.
using http3_received_data_credit_sink_type = void (*)(void* context, std::int64_t stream_id, std::size_t bytes) noexcept;

struct http3_connection_options final {
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
    // SETTINGS_H3_DATAGRAM (RFC 9297 §2.1.1): read_datagram() accepts HTTP/3
    // Datagrams. The QUIC owner must also advertise a nonzero
    // max_datagram_frame_size transport parameter (http3_quic_connection
    // does so whenever this is set).
    bool enable_h3_datagram_{false};
    // Copies DATA into event-owned storage. QUIC enables this because its
    // decrypted stream callback storage expires before receive_datagram returns.
    bool own_data_events_{false};
    // Server only: serialized origins (RFC 9412 §2) sent in an ORIGIN frame on
    // the control stream immediately after SETTINGS.
    borrowed_span<std::string_view> origins_{};
    // Where returned DATA credit goes; without a sink it is dropped.
    // http3_quic_connection installs its own and rejects one set here.
    http3_received_data_credit_sink_type received_data_credit_sink_{nullptr};
    void* received_data_credit_context_{nullptr};
    // Independent synchronous wire and QPACK observations.
    http3_frame_observer_type frame_observer_{nullptr};
    http3_field_observer_type field_observer_{nullptr};
    void* observer_context_{nullptr};
};

// HTTP/3 stream codec. QUIC owns stream identifiers, retransmission, flow
// control and packet I/O; this class owns QPACK and HTTP/3 control/request
// streams through nghttp3. With own_data_events_ disabled, input DATA views
// remain valid until next_event() has drained every event from the
// corresponding read_stream() call.
// A server validates every received request head (see http3_request_head)
// and a client every response head and trailer section; a malformed message
// is a stream error of type H3_MESSAGE_ERROR (RFC 9114 §4.1.2), which the
// codec answers with local STOP_SENDING and RESET_STREAM transport actions.
// After that no message event names the stream.
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
    [[nodiscard]] http3_transport_status bind_control_streams(
        std::int64_t control_stream_id, std::int64_t qpack_encoder_stream_id,
        std::int64_t qpack_decoder_stream_id);

    // ts_nanoseconds uses one monotonic clock and never decreases.
    [[nodiscard]] http3_transport_result read_stream(std::int64_t stream_id,
        std::span<const std::byte> bytes, bool fin, std::uint64_t ts_nanoseconds);
    template <detail::http_temporary_owning_range<std::byte> bytes_type>
        requires(!std::same_as<std::remove_cvref_t<bytes_type>, borrowed_span<std::byte>>)
    http3_transport_result read_stream(std::int64_t, bytes_type&&, bool, std::uint64_t) = delete;
    http3_transport_result read_stream(std::int64_t, std::initializer_list<std::byte>, bool, std::uint64_t) = delete;
    // Transport actions tell local commands (execute them) from peer
    // notifications (already applied) by http3_transport_action_event::origin().
    [[nodiscard]] std::optional<http3_event> next_event();

    // Borrows at most max_bytes of one HTTP/3 stream without advancing
    // nghttp3. The buffers remain valid through commit_output() and QUIC keeps
    // the underlying bytes alive until acknowledge_output().
    // QUIC reports exactly the accepted prefix through commit_output().
    [[nodiscard]] http3_transport_result peek_output(
        std::size_t max_bytes, http3_stream_output& output);
    [[nodiscard]] http3_transport_status commit_output(
        std::int64_t stream_id, std::size_t bytes) noexcept;
    [[nodiscard]] http3_transport_status acknowledge_output(
        std::int64_t stream_id, std::uint64_t bytes) noexcept;
    // Suspend scheduling a local send half when QUIC stream credit is
    // exhausted; unblock after QUIC grants credit. Neither changes credit,
    // commits bytes nor resumes an application body source.
    // Both return closed after a connection error.
    [[nodiscard]] http3_transport_status block_stream(std::int64_t stream_id) noexcept;
    [[nodiscard]] http3_transport_status unblock_stream(std::int64_t stream_id) noexcept;

    // Report termination of the local receive/send half, respectively.
    // With the peer's error code, publish a RESET_STREAM/STOP_SENDING
    // notification (http3_transport_action_origin::peer); a client turns
    // H3_REQUEST_REJECTED before the final response head into
    // http3_request_unprocessed_event instead. nullopt applies a local
    // termination without publishing a notification.
    // These calls do not send QUIC frames or close the opposite half;
    // the driver still reports final retirement through close_stream().
    // Both return closed after a connection error. A critical receive stream
    // termination can return protocol_error; connection_error() carries its
    // HTTP/3 error code.
    [[nodiscard]] http3_transport_status shutdown_stream_read(
        std::int64_t stream_id, std::optional<http3_error_code> peer_error = std::nullopt) noexcept;
    [[nodiscard]] http3_transport_status shutdown_stream_write(
        std::int64_t stream_id, std::optional<http3_error_code> peer_error = std::nullopt) noexcept;
    // Server only: absolute cumulative client-initiated bidirectional stream
    // limit granted by QUIC, not an increment or concurrent-stream count.
    // The transport supplies monotonically increasing limits; client is a no-op.
    void set_max_client_streams_bidi(std::uint64_t max_streams) noexcept;

    // QUIC allocates client-initiated bidirectional request stream
    // identifiers. Each head is validated as a whole, against the local and
    // known peer field section limits, before anything is published; the core
    // owns the framing fields. A head that leaves the stream open installs a
    // pull source that submit_data() and the finishing calls resume without
    // retaining caller views. invalid_state: not a client, not a client
    // bidirectional stream, or a head was already submitted on it.
    // invalid_message: the request cannot be sent as HTTP/3 (RFC 9114 §4.3.1,
    // RFC 9110). Extended CONNECT (RFC 9220) returns peer_settings_pending
    // before the peer SETTINGS (or 0-RTT remembered ones) are known and
    // peer_capability_unavailable when they do not enable it.
    [[nodiscard]] http3_submit_status submit_request_head(std::int64_t stream_id,
        const http3_regular_request_head_view& request);
    [[nodiscard]] http3_submit_status submit_request_head(std::int64_t stream_id,
        const http3_connect_request_head_view& request);
    [[nodiscard]] http3_submit_status submit_request_head(std::int64_t stream_id,
        const http3_extended_connect_request_head_view& request);
    // Copies the prefix that fits the stream's output budget; empty input is
    // complete without effect. A stream whose head declared a content length
    // returns invalid_message for DATA beyond it before copying anything.
    // Submitting to a stream whose content is not open (no head, a head that
    // ended the stream, or content already finished) throws std::logic_error.
    [[nodiscard]] http3_data_submit_result submit_data(
        std::int64_t stream_id, std::span<const std::byte> bytes);
    // DATA payload submitted on the stream and not yet acknowledged.
    [[nodiscard]] std::size_t pending_output_bytes(
        std::int64_t stream_id) const noexcept;
    // 当前排队的 HEADERS/DATA/尾部已全部交给 QUIC；不表示对端已确认，
    // 也不表示之后不能继续提交正文。已移除的流返回 true。
    [[nodiscard]] bool output_flushed(std::int64_t stream_id) const noexcept;
    // Remaining DATA capacity; unavailable and ended streams have no capacity.
    [[nodiscard]] std::size_t available_output_bytes(std::int64_t stream_id) const noexcept;
    // The finishing calls throw std::logic_error, like submit_data(), for
    // content that is not open. finish_stream() ends the content of either
    // role, including a tunnel.
    [[nodiscard]] http3_finish_status finish_stream(std::int64_t stream_id);
    // Client: ends the request content, with a trailer section when
    // `trailers` is nonempty. Trailer fields must be valid fields a sender may
    // place in request trailers (RFC 9110 §6.5.1) within the field section
    // limits.
    [[nodiscard]] http3_finish_status finish_request(std::int64_t stream_id,
        std::span<const http_header_view> trailers = {});

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
    // response (HEAD request, 204, 304) ends the stream with its head; the
    // request method is the one the stream received.
    [[nodiscard]] std::variant<http_response_stream_commit_plan, http3_response_head_error>
    submit_streaming_response_head(std::int64_t stream_id, http_response head,
        http_response_trailer_intent trailer_intent = http_response_trailer_intent::none);
    // A 1xx response before the final head (RFC 9114 §4.1). invalid_message:
    // the field section exceeds the local or peer field section limit.
    [[nodiscard]] http3_submit_status submit_interim_response_head(
        std::int64_t stream_id, const http_interim_response_head& response);
    // Ends a streaming response: an empty section ends the content, any other
    // section is sent as the trailing HEADERS frame. Rules as finish_stream().
    [[nodiscard]] http3_finish_status finish_response(std::int64_t stream_id,
        const http_response_trailer_section& trailers);

    // Accepts a CONNECT (RFC 9114 §4.4) or extended CONNECT (RFC 9220)
    // request with a 2xx head that frames no content (no Content-Length, no
    // Transfer-Encoding, no body). The stream then stays open in both
    // directions and carries the tunnel bytes as DATA (submit_data(),
    // finish_stream()). Any other answer takes submit_response_head().
    // invalid_state: the stream did not receive a CONNECT head or already
    // answered it; invalid_message: the head cannot accept a tunnel.
    [[nodiscard]] http3_submit_status submit_connect_response_head(
        std::int64_t stream_id, const http_response& response);
    // Accepts an RFC 9220 WebSocket bootstrap that
    // validate_http3_websocket_handshake() accepted: the 200 head carries the
    // negotiated subprotocol and permessage-deflate parameters, and the
    // stream continues as a tunnel like submit_connect_response_head().
    [[nodiscard]] std::variant<websocket_extended_connect_negotiation, http3_response_head_error>
    submit_websocket_handshake(std::int64_t stream_id, const http_request& request,
        const websocket_handshake_validation_result& validation,
        websocket_server_handshake_options options = {});

    // One QUIC DATAGRAM frame payload (RFC 9221) carrying an HTTP/3 Datagram.
    // A datagram for a stream whose head was not decoded, or that closed, is
    // dropped (RFC 9297 §2.1); a malformed one, or any while
    // enable_h3_datagram_ is off, is a connection error of type
    // H3_DATAGRAM_ERROR.
    [[nodiscard]] http3_transport_result read_datagram(std::span<const std::byte> payload);
    // The prefix of HTTP/3 Datagrams on `stream_id`, or nullopt unless both
    // endpoints enabled them (RFC 9297 §2.1.1) and the stream is a
    // client-initiated bidirectional stream this codec still tracks.
    [[nodiscard]] std::optional<http3_datagram_prefix> datagram_prefix(std::int64_t stream_id) const noexcept;
    // nullopt until the peer's SETTINGS frame arrived.
    [[nodiscard]] std::optional<http3_peer_settings> peer_settings() const noexcept;
    [[nodiscard]] bool header_block_in_progress() const noexcept;

    // Server only: queues an RFC 9412 ORIGIN frame on the bound control stream.
    [[nodiscard]] http3_submit_status submit_origin_advertisement(
        std::span<const std::string_view> origins);
    // Client only: queues an RFC 9218 PRIORITY_UPDATE frame on the bound
    // control stream for an open client-initiated bidirectional request stream.
    [[nodiscard]] http3_submit_status submit_priority_update(
        std::int64_t stream_id, http_priority_fields fields);
    // Server only: the current RFC 9218 priority of an open request stream.
    [[nodiscard]] std::optional<http_priority> stream_priority(
        std::int64_t stream_id) const noexcept;
    // Final retirement of a QUIC stream, with the application error codes of
    // the RESET_STREAM/STOP_SENDING that ended either half, if any.
    [[nodiscard]] http3_transport_status close_stream(std::int64_t stream_id,
        std::optional<http3_error_code> receive_error,
        std::optional<http3_error_code> send_error) noexcept;
    // Server graceful shutdown, RFC 9114 §5.2: begin_drain() sends the
    // shutdown-notice GOAWAY that still admits in-flight requests;
    // finish_drain(), at least one round trip later, sends the final GOAWAY
    // naming the first request stream that will not be processed. drained()
    // then reports that every admitted request stream has closed. Both return
    // closed after a connection error.
    [[nodiscard]] http3_submit_status begin_drain() noexcept;
    [[nodiscard]] http3_submit_status finish_drain() noexcept;
    [[nodiscard]] bool drain_final() const noexcept;
    [[nodiscard]] bool drained() const noexcept;
    [[nodiscard]] std::optional<http3_error_code> connection_error() const noexcept;

private:
    friend class http3_quic_connection;
    void begin_packet_input() noexcept;
    void end_packet_input() noexcept;
    void mark_early_data(std::int64_t stream_id);
    void remember_peer_settings(http3_peer_settings settings) noexcept;
    [[nodiscard]] bool events_pending() const noexcept;
    void rethrow_if_failed();
    class impl;
    explicit http3_connection(http3_role role,
        const http3_connection_options& options);
    std::unique_ptr<impl> impl_;
};

// RFC 9220 extended CONNECT with :protocol websocket on a client request
// stream: the negotiation's Sec-WebSocket-* request fields followed by the
// application headers. Validate the final head with
// validate_extended_connect_websocket_client_response().
[[nodiscard]] http3_submit_status submit_http3_websocket_client_request(http3_connection& connection,
    std::int64_t stream_id, const websocket_client_negotiation& negotiation, std::string_view scheme,
    std::string_view authority, std::string_view target, borrowed_http_headers headers = {},
    http_request_field_source source = http_request_field_source::generated);

}  // namespace ruvia

namespace ruvia {

enum class http3_quic_status : std::uint8_t {
    accepted,
    would_block,
    // The connection entered its closing or draining period (RFC 9000 §10.2):
    // a CONNECTION_CLOSE was sent or received, or the idle timeout expired.
    // No further packet is processed; the owner keeps the connection only to
    // let late peer packets drain until close_period() elapses.
    closed,
    invalid_state,
    transport_error,
    application_error,
};

// The error code of the CONNECTION_CLOSE that ended the connection, whether
// sent or received (RFC 9000 §19.19): transport_error and a transport close
// carry a QUIC transport error code (§20.1, 0x0100-0x01ff for a TLS alert);
// application_error and an application close carry an HTTP/3 code. Both are
// unset while the connection is open, and after an idle timeout or a
// stateless reset.
struct http3_quic_result final {
    http3_quic_status status_{http3_quic_status::accepted};
    std::optional<std::uint64_t> transport_error_code_{};
    std::optional<http3_error_code> application_error_code_{};
};

using http3_quic_stateless_reset_token =
    std::array<std::uint8_t, NGTCP2_STATELESS_RESET_TOKENLEN>;

// The QUIC-TLS adapter (for example ngtcp2_crypto_ossl's ngtcp2_crypto_*_cb).
// The connection installs these unchanged; they receive the connection's own
// user data, which the adapters do not read. client_initial_ and recv_retry_
// are client-only, recv_client_initial_ server-only; the others are required
// for both roles. The connection owns every stream, acknowledgement and
// close callback ngtcp2 defines, so none can be supplied.
struct http3_quic_crypto_callbacks final {
    ngtcp2_client_initial client_initial_{nullptr};
    ngtcp2_recv_client_initial recv_client_initial_{nullptr};
    ngtcp2_recv_crypto_data recv_crypto_data_{nullptr};
    ngtcp2_encrypt encrypt_{nullptr};
    ngtcp2_decrypt decrypt_{nullptr};
    ngtcp2_hp_mask hp_mask_{nullptr};
    ngtcp2_recv_retry recv_retry_{nullptr};
    ngtcp2_update_key update_key_{nullptr};
    ngtcp2_delete_crypto_aead_ctx delete_crypto_aead_ctx_{nullptr};
    ngtcp2_delete_crypto_cipher_ctx delete_crypto_cipher_ctx_{nullptr};
    ngtcp2_get_path_challenge_data2 get_path_challenge_data_{nullptr};
    ngtcp2_version_negotiation version_negotiation_{nullptr};
    ngtcp2_rand rand_{nullptr};
};

// Connection lifecycle notifications for the owner; each, when set, receives
// http3_quic_connection_options::callback_context_. A nonzero return fails
// the connection as ngtcp2 documents for the callback.
struct http3_quic_owner_callbacks final {
    // Issues a connection ID and its stateless reset token (RFC 9000 §5.1.1).
    ngtcp2_get_new_connection_id2 get_new_connection_id_{nullptr};
    ngtcp2_remove_connection_id remove_connection_id_{nullptr};
    ngtcp2_handshake_completed handshake_completed_{nullptr};
    ngtcp2_handshake_confirmed handshake_confirmed_{nullptr};
    ngtcp2_recv_stateless_reset2 recv_stateless_reset_{nullptr};
    // Runs after the connection discarded its rejected 0-RTT HTTP/3 streams.
    ngtcp2_tls_early_data_rejected tls_early_data_rejected_{nullptr};
};

struct http3_quic_connection_options final {
    http3_quic_crypto_callbacks crypto_callbacks_{};
    http3_quic_owner_callbacks owner_callbacks_{};
    ngtcp2_cid source_connection_id_{};
    ngtcp2_cid destination_connection_id_{};
    // Server only and required (RFC 9000 §7.3): the Destination Connection ID
    // of the client's first Initial packet (extracted from the Retry token
    // when Retry was used).
    std::optional<ngtcp2_cid> original_destination_connection_id_{};
    // Server only: the Source Connection ID sent in the Retry packet that the
    // client echoed as the Destination Connection ID of its second Initial
    // (RFC 9000 §7.3, §18.2). Requires retry_token_ to be non-empty.
    std::optional<ngtcp2_cid> retry_source_connection_id_{};
    // Server only: the validated Retry token from the client's second Initial
    // (RFC 9000 §8.1.2). Lifts the anti-amplification limit in ngtcp2.
    borrowed_span<std::uint8_t> retry_token_{};
    // Server only: the stateless reset token of source_connection_id_
    // (RFC 9000 §10.3), sent as a transport parameter.
    std::optional<http3_quic_stateless_reset_token> stateless_reset_token_{};
    // Server only: forbids client connection migration (RFC 9000 §9).
    // Leave false when the UDP owner routes connection IDs across paths.
    bool disable_active_migration_{false};
    ngtcp2_path initial_path_{};
    std::uint32_t version_{NGTCP2_PROTO_VER_V1};
    ngtcp2_tstamp initial_timestamp_{0};
    ngtcp2_duration handshake_timeout_{30 * NGTCP2_SECONDS};
    ngtcp2_duration idle_timeout_{30 * NGTCP2_SECONDS};
    std::uint64_t initial_max_data_{16 * 1024 * 1024};
    std::uint64_t initial_max_stream_data_{1024 * 1024};
    std::uint64_t initial_max_streams_bidi_{100};
    std::uint64_t initial_max_streams_uni_{16};
    // Leave spare CIDs for a new candidate while the current path and an
    // old-path validation coexist (RFC 9000 §5.1.1).
    std::uint64_t active_connection_id_limit_{4};
    std::size_t max_udp_payload_size_{1350};
    // Bounded unreliable output retained until ngtcp2 accepts each frame.
    std::size_t max_pending_datagram_bytes_{64 * 1024};
    void* callback_context_{nullptr};
    // The connection installs its own received_data_credit_sink_: a returned
    // http3_received_data_credit extends the stream and connection receive
    // windows (RFC 9000 §4.1), so DATA credit returns only as the application
    // consumes it. Framing bytes are credited at once.
    http3_connection_options http3_{};
};

// Socket addresses of one QUIC path (RFC 9000 §9), in the platform's
// sockaddr layout.
struct http3_quic_path final {
    std::span<const std::byte> local_;
    std::span<const std::byte> remote_;
};

struct http3_quic_datagram final {
    http3_quic_datagram() noexcept;
    http3_quic_datagram(const http3_quic_datagram&) = delete;
    http3_quic_datagram& operator=(const http3_quic_datagram&) = delete;
    http3_quic_datagram(http3_quic_datagram&&) = delete;
    http3_quic_datagram& operator=(http3_quic_datagram&&) = delete;

    ngtcp2_path_storage path_{};
    std::uint32_t ecn_{NGTCP2_ECN_NOT_ECT};
    std::vector<std::byte> bytes_;
};

// One QUIC v1 + HTTP/3 connection. ngtcp2 owns packet, loss recovery and flow
// control; nghttp3 owns QPACK and HTTP stream framing. The caller owns UDP and
// TLS. Input datagrams must remain alive until next_event() drains their events.
// Events can also be published while writing (a stream closing on an
// acknowledgement), so the owner drains next_event() after every
// receive_datagram(), next_datagram() and handle_expiry().
class http3_quic_connection final {
public:
    [[nodiscard]] static http3_quic_connection client(
        const http3_quic_connection_options& options);
    [[nodiscard]] static http3_quic_connection server(
        const http3_quic_connection_options& options);

    ~http3_quic_connection();
    http3_quic_connection(const http3_quic_connection&) = delete;
    http3_quic_connection& operator=(const http3_quic_connection&) = delete;
    http3_quic_connection(http3_quic_connection&&) noexcept;
    http3_quic_connection& operator=(http3_quic_connection&&) noexcept;

    [[nodiscard]] http3_role role() const noexcept;
    // For the QUIC-TLS adapter's connection reference
    // (ngtcp2_crypto_conn_ref::get_conn); every other capability has a member.
    [[nodiscard]] ngtcp2_conn* native_handle() noexcept;
    [[nodiscard]] const ngtcp2_conn* native_handle() const noexcept;
    [[nodiscard]] void* callback_context() const noexcept;
    void set_tls_native_handle(void* tls_native_handle) noexcept;
    [[nodiscard]] bool handshake_completed() const noexcept;
    [[nodiscard]] bool after_retry() const noexcept;
    [[nodiscard]] bool stateless_reset_received() const noexcept;

    // Opens and binds the local HTTP/3 control and QPACK streams after TLS has
    // established application keys. Safe to retry only when it would block.
    [[nodiscard]] http3_quic_status start_http3();
    [[nodiscard]] bool http3_started() const noexcept;
    [[nodiscard]] std::optional<std::int64_t> open_request_stream();

    // Restore authenticated transport parameters and peer SETTINGS from a
    // ticket before opening 0-RTT streams. TLS must enable early data too.
    [[nodiscard]] http3_quic_status start_early_http3(
        std::span<const std::byte> transport_parameters, http3_peer_settings settings);
    // On TLS early rejection, discard the rejected HTTP/3 streams before a
    // caller explicitly retries requests in 1-RTT; never replays requests.
    [[nodiscard]] http3_quic_status reject_early_data();
    // Client: TLS rejected the early data, whether reported through
    // reject_early_data() or by the QUIC-TLS adapter during the handshake.
    [[nodiscard]] bool early_data_rejected() const noexcept;
    // The transport parameters 0-RTT depends on (RFC 9000 §7.4.1). A client
    // gets the server's, once received, to store with the TLS session ticket
    // and restore with start_early_http3(); a server gets its own, which a
    // ticket's resumption check must bind. Returns the encoded size, or
    // nullopt when a client has not received them yet or `output` is too
    // small.
    [[nodiscard]] std::optional<std::size_t> encode_early_transport_parameters(
        std::span<std::byte> output) const noexcept;
    // The path the connection currently sends on, after any validated
    // migration (RFC 9000 §9). The views borrow connection storage until the
    // next call that processes input, writes output or migrates.
    [[nodiscard]] http3_quic_path current_path() const& noexcept RUVIA_LIFETIMEBOUND;
    http3_quic_path current_path() const&& = delete;
    // Client only, QUIC v1: validate a new path before switching. The caller
    // must send packets on output.path_ and keep both paths receiving.
    [[nodiscard]] http3_quic_result migrate(const ngtcp2_path& path, ngtcp2_tstamp timestamp);
    // Payload follows the HTTP Datagram Quarter Stream ID. Owned and bounded;
    // invalid_state means unsupported/oversized, would_block means queue full.
    [[nodiscard]] http3_quic_status submit_datagram(
        std::int64_t stream_id, std::span<const std::byte> payload);
    [[nodiscard]] std::size_t max_datagram_payload_size(std::int64_t stream_id) const noexcept;
    [[nodiscard]] http3_quic_result receive_datagram(const ngtcp2_path& path,
        std::uint32_t ecn, std::span<const std::byte> bytes,
        ngtcp2_tstamp timestamp);
    // While the connection is open: the next packet, or would_block. After a
    // local close: the CONNECTION_CLOSE packet once, then closed.
    [[nodiscard]] http3_quic_result next_datagram(std::size_t capacity,
        ngtcp2_tstamp timestamp, http3_quic_datagram& output);
    [[nodiscard]] http3_quic_result handle_expiry(ngtcp2_tstamp timestamp);
    [[nodiscard]] ngtcp2_tstamp expiry() const noexcept;

    // Abandons both directions of a request stream with RESET_STREAM and
    // STOP_SENDING carrying `error` (RFC 9114 §4.1.1).
    void reset_stream(std::int64_t stream_id, http3_error_code error) noexcept;
    // Ends only the sending half of a stream with RESET_STREAM carrying
    // `error`; its receiving half keeps delivering (RFC 9000 §3.1,
    // RFC 9114 §4.1).
    void reset_send_stream(std::int64_t stream_id, http3_error_code error) noexcept;
    // Stops reading a request whose response no longer needs the rest of its
    // content (RFC 9114 §4.1.2 permits H3_NO_ERROR once the response is
    // complete).
    void stop_receiving(std::int64_t stream_id, http3_error_code error) noexcept;
    // Requests an orderly close: the next next_datagram() writes a
    // CONNECTION_CLOSE carrying `error` (RFC 9000 §10.2; H3_NO_ERROR for a
    // graceful HTTP/3 shutdown, RFC 9114 §5.2).
    void close(http3_error_code error) noexcept;
    // The closing or draining period began (see http3_quic_status::closed).
    [[nodiscard]] bool closed() const noexcept;
    // How long a closed connection keeps absorbing late packets: three times
    // the current probe timeout (RFC 9000 §10.2).
    [[nodiscard]] ngtcp2_duration close_period() const noexcept;

    [[nodiscard]] std::optional<http3_event> next_event();
    [[nodiscard]] http3_connection& http3() noexcept;
    [[nodiscard]] const http3_connection& http3() const noexcept;
    // The HTTP/3 code of the CONNECTION_CLOSE this connection sent or
    // received, or of the codec error it failed on.
    [[nodiscard]] std::optional<http3_error_code> application_error() const noexcept;
    // The QUIC transport error code (RFC 9000 §20.1) of the CONNECTION_CLOSE
    // this connection sent or received.
    [[nodiscard]] std::optional<std::uint64_t> transport_error() const noexcept;

private:
    class impl;
    explicit http3_quic_connection(http3_role role,
        const http3_quic_connection_options& options);
    std::unique_ptr<impl> impl_;
};

// Writes a QUIC Stateless Reset packet (RFC 9000 §10.3) into `output`, using
// at most `max_packet_bytes` (pass `trigger_datagram_bytes - 1` so the packet
// is strictly smaller than the datagram that triggered it, RFC 9000 §10.3.3).
// Returns the number of bytes written, or 0 when `output`, `max_packet_bytes`
// or `random_bytes` cannot form a valid Stateless Reset.
[[nodiscard]] std::size_t write_http3_quic_stateless_reset(
    const http3_quic_stateless_reset_token& token,
    std::span<const std::uint8_t> random_bytes,
    std::span<std::byte> output,
    std::size_t max_packet_bytes) noexcept;

}  // namespace ruvia
