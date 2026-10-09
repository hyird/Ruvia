#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace ruvia {

enum class http2_finish_request_status : std::uint8_t {
    accepted,
    queued,
    closed,
    invalid_state,
    content_length_incomplete,
    invalid_trailer,
};

enum class http2_role : std::uint8_t {
    server,
    client,
};

// Read-only receive-side state for one stream; never exposes stream storage.
enum class http2_stream_receive_status : std::uint8_t {
    open,
    ended,
    closed,
};

// Borrowed snapshot of the peer's HTTP/2 request metadata. Views remain valid
// only until the connection consumes more input; no stream storage is exposed.
struct http2_server_request_view final {
    std::string_view method_{};
    std::string_view path_{};
    std::string_view authority_{};
    std::string_view protocol_{};
};

// Read-only send-flow-control observation for a live stream. The available
// amount is the minimum of the connection and stream windows, clamped at zero.
struct http2_send_window_state final {
    std::int32_t connection_window_{0};
    std::int32_t stream_window_{0};
    std::uint32_t available_{0};
    bool queued_data_{false};
};

// feed() has all-or-nothing ownership for each supplied span; it never partially
// consumes caller input. The public facade always begins the connection, so it
// never returns connection_not_started; the sans-I/O core can, and the caller
// retries the same span after begin_connection().
enum class http2_feed_result : std::uint8_t {
    connection_not_started,
    events_pending,
    accepted,
    need_input,
    protocol_failure,
};

enum class http2_end_stream : std::uint8_t {
    keep_open,
    end_stream,
};

[[nodiscard]] constexpr bool http2_ends_stream(http2_end_stream value) noexcept {
    return value == http2_end_stream::end_stream;
}

enum class http2_output_consume_status : std::uint8_t {
    pending,
    drained,
    out_of_range,
};

enum class http2_output_batch_status : std::uint8_t { taken,
    empty,
    unaligned };

struct http2_output_batch_result final {
    http2_output_batch_status status_{http2_output_batch_status::empty};
    std::size_t bytes_{0};
};

using http2_data_output_observer_type = void (*)(void*, std::uint32_t, std::size_t) noexcept;

// Initial-head/control submission status. closed is an expected race with a
// reset peer; invalid_state is a caller contract violation and emits no bytes.
// queued/backpressured belong on http2_data_submit_status, not here.
enum class http2_submit_status : std::uint8_t {
    accepted,
    closed,
    invalid_state,
    invalid_message,
    peer_capability_unavailable,
};

enum class http2_data_queue_state : std::uint8_t { drained,
    queued,
    aborted };

enum class http2_data_submit_status : std::uint8_t {
    accepted,
    queued,
    backpressured,
    expectation_pending,
    closed,
    invalid_state,
    content_length_exceeded,
    content_length_incomplete,
};

enum class http2_request_content_release_status : std::uint8_t {
    released,
    not_pending,
    closed,
};

enum class http2_stream_close_source : std::uint8_t {
    local,
    peer,
    peer_goaway,
};

[[nodiscard]] constexpr bool http2_is_valid_stream_close_source(http2_stream_close_source source_value) noexcept {
    return source_value == http2_stream_close_source::local || source_value == http2_stream_close_source::peer ||
           source_value == http2_stream_close_source::peer_goaway;
}

enum class http2_request_head_submit_error : std::uint8_t {
    invalid_state,
    connection_not_started,
    connection_unavailable,
    peer_stream_limit_reached,
    local_stream_capacity_reached,
    peer_capability_unavailable,
    invalid_message,
};

enum class http2_response_head_submit_error : std::uint8_t {
    peer_stream_limit_reached,
    closed,
    invalid_state,
    response_plan_mismatch,
    invalid_message,
};

}  // namespace ruvia
