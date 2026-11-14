#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include "borrowed_text.h"
#include "http_client_request_view.h"
#include "http_header.h"
#include "http_request.h"

// Vocabulary shared by the HTTP/2 and HTTP/3 stream engines: one shape for
// the request heads, submission outcomes and receive flow-control credit.
// Protocol-only enumerators are documented on the enumerator.
namespace ruvia {

enum class http_connection_role : std::uint8_t {
    client,
    server,
};

struct http_regular_request_head_view final {
    borrowed_text method_{"GET"};
    borrowed_text scheme_{"https"};
    std::optional<borrowed_text> authority_{};
    borrowed_text target_{"/"};
    borrowed_http_headers headers_{};
    http_request_content_plan content_{http_request_content_plan::none()};
    http_client_request_expectation expectation_{http_client_request_expectation::none};
    http_request_field_source field_source_{http_request_field_source::generated};
};

struct http_connect_request_head_view final {
    borrowed_text authority_{};
    borrowed_http_headers headers_{};
    http_request_field_source field_source_{http_request_field_source::generated};
};

struct http_extended_connect_request_head_view final {
    borrowed_text protocol_{};
    borrowed_text scheme_{"https"};
    borrowed_text authority_{};
    borrowed_text target_{"/"};
    borrowed_http_headers headers_{};
    http_request_field_source field_source_{http_request_field_source::generated};
};

// Head and control submissions. closed is an expected race with a reset
// stream or a failed connection; invalid_state is a request the role or
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

}  // namespace detail

}  // namespace ruvia
