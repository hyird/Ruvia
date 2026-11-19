#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "borrowed_text.h"
#include "http_client_response_head.h"
#include "http_header.h"
#include "http_request_head_view.h"

// The stream-level contract shared by http2_connection and http3_connection:
// one request head view, one set of submission outcomes, one shape for every
// stream event both protocols publish, and the receive flow-control credit.
// Protocol-only enumerators are documented on the enumerator.
// Both engines name their drain state alike, with these exact meanings:
// - draining(): this endpoint began a drain (begin_drain() / finish_drain())
//   or the peer sent GOAWAY.
// - drain_final(): the final local GOAWAY is queued. HTTP/2 also enters it on
//   a valid peer GOAWAY; HTTP/3 only through its own finish_drain().
// - drained(): drain_final() and no stream it admitted remains. HTTP/2 counts
//   every tracked stream, owner leases included; HTTP/3 counts the
//   client-initiated request streams below its final GOAWAY. It therefore
//   becomes true after a peer GOAWAY on HTTP/2 but never on HTTP/3.
namespace ruvia {

enum class http_connection_role : std::uint8_t {
    client,
    server,
};

// Head and control submissions. closed is an expected race with a reset
// stream or a failed connection; for a new request it also covers a
// connection that admits no further stream (a peer GOAWAY, or exhausted
// stream identifiers). invalid_state is a request the role or
// stream state cannot take and emits nothing. invalid_message is a message
// the protocol cannot carry or that exceeds a field section limit, returned
// so a caller forwarding peer data can answer it.
// peer_capability_unavailable: the peer's SETTINGS do not enable the feature.
enum class http_submit_status : std::uint8_t {
    accepted,
    closed,
    invalid_state,
    invalid_message,
    peer_capability_unavailable,
    // HTTP/3 only: the operation depends on peer SETTINGS that have not
    // arrived (RFC 9114 §7.2.4); retry after peer_settings() reports them.
    peer_settings_pending,
    // HTTP/3 only: submit_origin_advertisement() found the control stream's
    // output budget full; retry after acknowledgements free it.
    backpressured,
    // HTTP/2 only: submit_request_head() found no slot under the peer's
    // SETTINGS_MAX_CONCURRENT_STREAMS (RFC 9113 §5.1.2); retry after a
    // request stream closes.
    peer_stream_limit_reached,
    // HTTP/2 only: submit_request_head() found the fixed local stream
    // capacity in use; retry after a request stream closes.
    local_stream_capacity_reached,
};

enum class http_finish_status : std::uint8_t {
    accepted,
    // The stream disappeared, was reset, or its send half was abandoned.
    closed,
    // The submitted DATA does not reach the declared Content-Length, or the
    // trailer section cannot be sent. Nothing changed; the stream stays open.
    invalid_message,
    // HTTP/2 only: an empty trailer section ends the stream with an empty DATA
    // frame, sent only with positive connection and stream send windows
    // (RFC 9113 §6.9). Nothing changed; retry after the window opens.
    backpressured,
};

// complete: the whole input was consumed (not that it reached the socket).
// backpressured: only consumed_bytes_ (possibly none) fit the send window or
// output budget; the caller keeps the rest and retries.
// closed: the stream is gone or its send half was abandoned.
// invalid_message: the input would exceed the declared Content-Length, or end
// the stream short of it. Nothing was consumed.
enum class http_data_submit_status : std::uint8_t {
    complete,
    backpressured,
    // HTTP/2 only: request content is held until 100 (Continue).
    expectation_pending,
    closed,
    invalid_message,
};

// Only consumed_bytes_ were copied into serialized DATA; the unconsumed suffix
// stays caller-owned and no input view is retained.
struct http_data_submit_result final {
    std::size_t consumed_bytes_{0};
    http_data_submit_status status_{http_data_submit_status::complete};
};

// Failure alternative of every value-carrying response-head submission.
enum class http_response_head_error : std::uint8_t {
    // The stream no longer exists or the connection failed.
    closed,
    // A final head was already submitted, or the role or stream cannot take it.
    invalid_state,
    // The response cannot be represented (connection-specific fields, a
    // non-final status, an invalid Content-Length or Transfer-Encoding).
    invalid_message,
    // The write plan does not describe this response.
    response_plan_mismatch,
    // HTTP/2 only: a pushed stream found no slot under the peer's
    // SETTINGS_MAX_CONCURRENT_STREAMS (RFC 9113 §8.4).
    peer_stream_limit_reached,
};

// Observation budget of the HTTP/2 and HTTP/3 fingerprint collectors: a
// collector fails once it has seen more frames, or more frame and field
// bytes, before the first request header block completes.
struct http_fingerprint_options final {
    std::size_t max_frames_{32};
    std::size_t max_bytes_{16384};
};

class http_received_data_credit;

namespace detail {

// Where escaped credits return. Intrusively, non-atomically reference-counted
// (credits are worker-affine) so a credit may outlive its connection.
class http_received_data_target {
public:
    http_received_data_target(const http_received_data_target&) = delete;
    http_received_data_target& operator=(const http_received_data_target&) = delete;

    void retain() noexcept {
        ++references_;
    }
    void release() noexcept;
    // Returns exactly `bytes` of `stream_id`; must not throw.
    virtual void return_credit(std::uint64_t stream_id, std::size_t bytes) noexcept = 0;

protected:
    http_received_data_target() noexcept = default;
    virtual ~http_received_data_target() = default;

private:
    std::size_t references_{1};
};

struct http_received_data_credit_access;

}  // namespace detail

// Receive flow-control credit (RFC 9113 §6.9, RFC 9000 §4.1) for the DATA
// payload of one event. A credit is linear and has one return path:
// destroying a still-valid token returns its exact byte count to the issuing
// connection without throwing, so a consumer holds it until the bytes are
// consumed. The token keeps its target alive and may outlive the connection;
// a return to a stream or connection that is already gone is dropped.
class http_received_data_credit final {
public:
    ~http_received_data_credit();
    http_received_data_credit(const http_received_data_credit&) = delete;
    http_received_data_credit& operator=(const http_received_data_credit&) = delete;
    http_received_data_credit(http_received_data_credit&& other) noexcept;
    http_received_data_credit& operator=(http_received_data_credit&&) = delete;
    // Combines credits of one stream of one connection without returning any
    // bytes. Success consumes other; an invalid or mismatched token throws
    // std::logic_error, a byte count overflow std::overflow_error.
    void merge(http_received_data_credit&& other);
    [[nodiscard]] bool valid() const noexcept {
        return target_ != nullptr && bytes_ != 0;
    }

private:
    friend struct detail::http_received_data_credit_access;
    http_received_data_credit(detail::http_received_data_target* target, std::uint64_t stream_id, std::size_t bytes) noexcept;
    detail::http_received_data_target* target_{nullptr};
    std::uint64_t stream_id_{0};
    std::size_t bytes_{0};
};

namespace detail {

struct http_received_data_credit_access final {
    [[nodiscard]] static http_received_data_credit make(http_received_data_target* target, std::uint64_t stream_id, std::size_t bytes) noexcept {
        return http_received_data_credit(target, stream_id, bytes);
    }
    static void swap(http_received_data_credit& left, http_received_data_credit& right) noexcept;
};

// Stream events are produced by the stream engines only.
struct http_stream_event_access final {
    template <typename event_type, typename... argument_types>
    [[nodiscard]] static event_type make(argument_types&&... arguments) {
        return event_type(std::forward<argument_types>(arguments)...);
    }
};

}  // namespace detail

// The stream events http2_connection and http3_connection both publish, as
// one class template per shape. Each protocol header names its
// instantiations (http2_data_event, http3_data_event, ...); the stream
// identifier keeps the protocol's type (RFC 9113 §5.1.1, RFC 9000 §2.1).

// Client: a 1xx response head. request_content_signal() is continue_ for the
// first 100 (Continue) of a request submitted with Expect: 100-continue.
template <typename stream_id_type>
class http_stream_informational_head_event final {
public:
    http_stream_informational_head_event(http_stream_informational_head_event&&) noexcept = default;
    http_stream_informational_head_event& operator=(http_stream_informational_head_event&&) = delete;
    [[nodiscard]] stream_id_type stream_id() const noexcept {
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
    friend struct detail::http_stream_event_access;
    http_stream_informational_head_event(stream_id_type stream_id, http_client_response_head head, std::optional<http_client_request_content_signal> signal) noexcept :
        stream_id_(stream_id),
        head_(std::move(head)),
        signal_(signal) {}
    stream_id_type stream_id_;
    http_client_response_head head_;
    std::optional<http_client_request_content_signal> signal_;
};

// Client: the final response head. Fields are validated regular fields; the
// raw three-digit status is retained (received_status()) and status() applies
// RFC 9110 §15 semantics (600..999 process as 500). request_content_signal()
// is exchange_complete when the final head arrived while request content
// still waited for 100 (Continue). stream_ended(): the HEADERS frame ended
// the stream, so no content or tunnel DATA follows this head. It is fixed
// when the head is published; a later frame that ends the stream never sets
// it.
template <typename stream_id_type>
class http_stream_response_head_event final {
public:
    http_stream_response_head_event(http_stream_response_head_event&&) noexcept = default;
    http_stream_response_head_event& operator=(http_stream_response_head_event&&) = delete;
    [[nodiscard]] stream_id_type stream_id() const noexcept {
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
    friend struct detail::http_stream_event_access;
    http_stream_response_head_event(stream_id_type stream_id, http_client_response_head head, std::optional<http_client_request_content_signal> signal, bool stream_ended) noexcept :
        stream_id_(stream_id),
        head_(std::move(head)),
        signal_(signal),
        stream_ended_(stream_ended) {}
    stream_id_type stream_id_;
    http_client_response_head head_;
    std::optional<http_client_request_content_signal> signal_;
    bool stream_ended_;
};

// What the DATA payload of an event is. One class carries both; the kind is
// part of the event type, so an event variant tells them apart.
enum class http_stream_data_kind : std::uint8_t {
    // Content of a request or response message. HTTP/3 publishes the bytes
    // of a CONNECT tunnel with this kind too.
    message,
    // HTTP/2 only: bytes of a CONNECT tunnel, including those a server
    // receives before it accepts or rejects the tunnel.
    tunnel,
};

// DATA payload of one stream. The bytes borrow the engine input that produced
// the event unless the engine was asked to copy them into the event
// (http3_connection_options::own_data_events_). The credit stays with the
// event until take_credit() hands it to the consumer.
// Move assignment returns the target's untaken credit.
template <typename stream_id_type, http_stream_data_kind data_kind>
class http_stream_data_event final {
public:
    static constexpr http_stream_data_kind kind = data_kind;

    http_stream_data_event(const http_stream_data_event&) = delete;
    http_stream_data_event& operator=(const http_stream_data_event&) = delete;
    http_stream_data_event(http_stream_data_event&& other) noexcept :
        stream_id_(other.stream_id_),
        bytes_(other.bytes_),
        owned_bytes_(std::move(other.owned_bytes_)),
        owns_bytes_(other.owns_bytes_),
        credit_(std::move(other.credit_)) {
        if (owns_bytes_) {
            bytes_ = owned_bytes_;
        }
    }
    http_stream_data_event& operator=(http_stream_data_event&& other) noexcept {
        if (this == &other) {
            return *this;
        }
        owned_bytes_ = std::move(other.owned_bytes_);
        owns_bytes_ = other.owns_bytes_;
        bytes_ = owns_bytes_ ? std::string_view(owned_bytes_) : other.bytes_;
        stream_id_ = other.stream_id_;
        http_received_data_credit incoming(std::move(other.credit_));
        detail::http_received_data_credit_access::swap(credit_, incoming);
        return *this;
    }

    [[nodiscard]] stream_id_type stream_id() const noexcept {
        return stream_id_;
    }
    [[nodiscard]] bool owns_bytes() const noexcept {
        return owns_bytes_;
    }
    [[nodiscard]] std::string_view bytes() const& noexcept RUVIA_LIFETIMEBOUND {
        return bytes_;
    }
    std::string_view bytes() const&& = delete;
    [[nodiscard]] std::string take_bytes() && {
        if (owns_bytes_) {
            owns_bytes_ = false;
            bytes_ = {};
            return std::exchange(owned_bytes_, {});
        }
        std::string copied(bytes_);
        bytes_ = {};
        return copied;
    }
    http_received_data_credit take_credit() & = delete;
    [[nodiscard]] http_received_data_credit take_credit() && noexcept {
        return std::move(credit_);
    }

private:
    friend struct detail::http_stream_event_access;
    http_stream_data_event(stream_id_type stream_id, std::string_view bytes, bool own_bytes, http_received_data_credit credit) :
        stream_id_(stream_id),
        bytes_(bytes),
        owns_bytes_(own_bytes),
        credit_(std::move(credit)) {
        if (owns_bytes_) {
            owned_bytes_.assign(bytes);
            bytes_ = owned_bytes_;
        }
    }
    stream_id_type stream_id_;
    std::string_view bytes_;
    std::string owned_bytes_;
    bool owns_bytes_{false};
    http_received_data_credit credit_;
};

// Whether a completed message carried content semantics. metadata_only: a
// response whose Content-Length describes a representation it does not carry
// (a HEAD response, 204 or 304), so no content decoding applies.
enum class http_message_content_semantics : std::uint8_t {
    content,
    metadata_only,
};

// The peer ended its send half normally: the request or response content is
// complete (HTTP/3: or a tunnel's receive direction ended). trailers() is the
// trailer section that preceded the end, empty when there was none, with
// lowercase field names (RFC 9113 §8.2.2, RFC 9114 §4.2). A client receives
// validated fields with those RFC 9110 §6.5.1 forbids in response trailers
// already discarded; a server passes them to
// http_request_trailers::append_received(). The span borrows the event: it
// is invalidated when the event is moved or destroyed, or once
// take_trailers() moved the fields out.
template <typename stream_id_type>
class http_stream_message_end_event final {
public:
    http_stream_message_end_event(http_stream_message_end_event&&) noexcept = default;
    http_stream_message_end_event& operator=(http_stream_message_end_event&&) = delete;
    [[nodiscard]] stream_id_type stream_id() const noexcept {
        return stream_id_;
    }
    [[nodiscard]] std::span<const http_header> trailers() const& noexcept RUVIA_LIFETIMEBOUND {
        return trailers_;
    }
    std::span<const http_header> trailers() const&& = delete;
    [[nodiscard]] std::vector<http_header> take_trailers() && noexcept {
        return std::move(trailers_);
    }
    [[nodiscard]] http_message_content_semantics content_semantics() const noexcept {
        return content_semantics_;
    }

private:
    friend struct detail::http_stream_event_access;
    http_stream_message_end_event(stream_id_type stream_id, std::vector<http_header> trailers, http_message_content_semantics content_semantics) noexcept :
        stream_id_(stream_id),
        trailers_(std::move(trailers)),
        content_semantics_(content_semantics) {}
    stream_id_type stream_id_;
    std::vector<http_header> trailers_;
    http_message_content_semantics content_semantics_;
};

// Which endpoint abandoned a stream, when the engine itself observed it.
enum class http_stream_close_source : std::uint8_t {
    // This endpoint: a stream error the engine detected.
    local,
    // The peer reset the stream.
    peer,
    // HTTP/2 only: a peer GOAWAY excluded the stream (RFC 9113 §6.8).
    peer_goaway,
};

// A stream ended other than by a completed message exchange. error() is the
// protocol error code of the reset that ended it. source() names the endpoint
// that abandoned it when the engine knows:
// - HTTP/2 publishes this event only for an abandoned stream, always with
//   both values (the code may be NO_ERROR).
// - HTTP/3 publishes it for every stream QUIC retires, once both directions
//   are closed; error() is absent after a clean close. QUIC owns stream
//   termination, so source() is absent and the preceding
//   http3_transport_action_event::origin() names the endpoint.
// No further event names the stream, and a stream reported as
// request-unprocessed publishes no close.
template <typename stream_id_type, typename error_code_type>
class http_stream_closed_event final {
public:
    [[nodiscard]] constexpr stream_id_type stream_id() const noexcept {
        return stream_id_;
    }
    [[nodiscard]] constexpr std::optional<error_code_type> error() const noexcept {
        return error_;
    }
    [[nodiscard]] constexpr std::optional<http_stream_close_source> source() const noexcept {
        return source_;
    }

private:
    friend struct detail::http_stream_event_access;
    constexpr http_stream_closed_event(stream_id_type stream_id, std::optional<error_code_type> error, std::optional<http_stream_close_source> source) noexcept :
        stream_id_(stream_id),
        error_(error),
        source_(source) {}
    stream_id_type stream_id_;
    std::optional<error_code_type> error_;
    std::optional<http_stream_close_source> source_;
};

// Client only: a request stream this endpoint opened that the server did not
// process, so the request may be retried (RFC 9113 §8.7, RFC 9114 §4.1.1 and
// §5.2). Published, after the GOAWAY event, for every open request stream the
// GOAWAY excludes, and for a stream the server refused before its final
// response head (RST_STREAM(REFUSED_STREAM), H3_REQUEST_REJECTED); the latter
// leaves the connection usable. No message event or stream close names the
// stream after it.
template <typename stream_id_type>
class http_stream_request_unprocessed_event final {
public:
    [[nodiscard]] constexpr stream_id_type stream_id() const noexcept {
        return stream_id_;
    }

private:
    friend struct detail::http_stream_event_access;
    explicit constexpr http_stream_request_unprocessed_event(stream_id_type stream_id) noexcept :
        stream_id_(stream_id) {}
    stream_id_type stream_id_;
};

}  // namespace ruvia
