#pragma once
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/http2_framing.h"
#include "ruvia/http/websocket_protocol.h"
namespace ruvia {
class http2_connection;
namespace detail {
class http2_connection;
}  // namespace detail

enum class http2_finish_status : std::uint8_t {
    accepted,
    queued,
    // The stream disappeared or was reset while its owner was finishing it.
    closed,
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

enum class http2_request_pseudo_header : std::uint8_t {
    method,
    scheme,
    authority,
    path,
    protocol,
};

// 对端请求的借用视图；通过 request-head event 取得，存储由该 lease 保持。
// 连接移动不使已有 lease 的视图失效。
struct http2_server_request_view final {
    std::string_view method_{};
    std::string_view scheme_{};
    std::string_view path_{};
    std::string_view authority_{};
    std::string_view protocol_{};
    // RFC 9113 §8.3：只包含线上实际出现的伪首部，保持解码顺序。
    // 不补入由 Host 推导的 authority；本地合成的 push 请求为空。
    std::span<const http2_request_pseudo_header> pseudo_header_order_{};
};

struct http2_setting_entry final {
    http2_setting_id id_{http2_setting_id::header_table_size};
    std::uint32_t value_{0};
};

// received_settings 事件：已应用的非 ACK SETTINGS 的借用视图，保留未知项、
// 重复项和线上顺序。借用已接受的 feed() 输入，与其他事件派生的字节视图一样，
// 在下一次 feed() 前有效；不保存连接历史，也不分配条目数组。
class http2_received_settings final {
public:
    http2_received_settings() = default;
    [[nodiscard]] std::size_t size() const noexcept {
        return payload_.size() / 6;
    }
    // 越界抛 std::out_of_range。
    [[nodiscard]] http2_setting_entry at(std::size_t index) const;

private:
    friend class detail::http2_connection;
    explicit http2_received_settings(std::string_view payload) noexcept
        : payload_(payload) {}
    std::string_view payload_{};
};

// received_window_update 事件：连接实际应用的 WINDOW_UPDATE（stream 0 或存活流）。
struct http2_received_window_update final {
    std::uint32_t stream_id_{0};
    std::uint32_t increment_{0};
};

// received_priority 事件：RFC 9113 §6.2/§6.3 的原始 dependency/weight 元数据，
// 来自 PRIORITY 帧或带 PRIORITY 标志的 HEADERS 帧，不代表调度策略。
struct http2_received_priority final {
    std::uint32_t stream_id_{0};
    std::uint32_t stream_dependency_{0};
    std::uint16_t weight_{0};
    bool exclusive_{false};
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
// consumes caller input.
enum class http2_feed_result : std::uint8_t {
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

enum class http2_output_batch_status : std::uint8_t { taken,
    empty };

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
enum class http2_websocket_handshake_submit_error : std::uint8_t { closed,
    invalid_state };

// The negotiation committed on the wire by the RFC 8441 200 response. The
// runtime configures its websocket connection from this value, never from a
// separately recomputed compression/subprotocol tuple.
class http2_websocket_negotiation final {
public:
    [[nodiscard]] std::string_view subprotocol() const& noexcept {
        return subprotocol_;
    }
    std::string_view subprotocol() const&& = delete;
    [[nodiscard]] websocket_compression compression() const noexcept {
        return compression_;
    }

private:
    friend class http2_connection;
    friend class detail::http2_connection;
    http2_websocket_negotiation(std::pmr::string subprotocol, websocket_compression compression) noexcept
        : subprotocol_(std::move(subprotocol)),
          compression_(compression) {}
    std::pmr::string subprotocol_;
    websocket_compression compression_;
};

}  // namespace ruvia
