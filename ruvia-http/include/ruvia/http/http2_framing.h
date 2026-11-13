#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace ruvia {

inline constexpr std::string_view http2_client_preface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
inline constexpr std::size_t http2_frame_header_bytes = 9;
inline constexpr std::uint32_t http2_default_max_frame_size = 16 * 1024;
inline constexpr std::uint32_t http2_max_frame_size = 16 * 1024 * 1024 - 1;

enum class http2_frame_type : std::uint8_t {
    data = 0x0,
    headers = 0x1,
    priority = 0x2,
    rst_stream = 0x3,
    settings = 0x4,
    push_promise = 0x5,
    ping = 0x6,
    goaway = 0x7,
    window_update = 0x8,
    continuation = 0x9,
    alternative_service = 0xa,
    origin = 0xc,
    priority_update = 0x10
};
enum class http2_error_code : std::uint32_t {
    no_error = 0x0,
    protocol_error = 0x1,
    internal_error = 0x2,
    flow_control_error = 0x3,
    settings_timeout = 0x4,
    stream_closed = 0x5,
    frame_size_error = 0x6,
    refused_stream = 0x7,
    cancel = 0x8,
    compression_error = 0x9,
    connect_error = 0xa,
    enhance_your_calm = 0xb,
    inadequate_security = 0xc,
    http11_required = 0xd
};
enum class http2_setting_id : std::uint16_t {
    header_table_size = 0x1,
    enable_push = 0x2,
    max_concurrent_streams = 0x3,
    initial_window_size = 0x4,
    max_frame_size = 0x5,
    max_header_list_size = 0x6,
    enable_connect_protocol = 0x8,
    no_rfc7540_priorities = 0x9
};

struct http2_frame_header final {
    std::uint32_t length_{0};
    std::uint8_t type_{0};
    std::uint8_t flags_{0};
    std::uint32_t stream_id_{0};
};

// RFC 9113 section 4.1. The reserved stream-identifier bit is ignored.
[[nodiscard]] inline constexpr std::optional<http2_frame_header> parse_http2_frame_header(
    std::span<const char> bytes_value) noexcept {
    if (bytes_value.size() < http2_frame_header_bytes) {
        return std::nullopt;
    }
    const auto byte = [bytes_value](std::size_t index) constexpr noexcept {
        return static_cast<std::uint32_t>(static_cast<unsigned char>(bytes_value[index]));
    };
    return http2_frame_header{
        .length_ = (byte(0) << 16) | (byte(1) << 8) | byte(2),
        .type_ = static_cast<std::uint8_t>(bytes_value[3]),
        .flags_ = static_cast<std::uint8_t>(bytes_value[4]),
        .stream_id_ = ((byte(5) & 0x7fU) << 24) | (byte(6) << 16) | (byte(7) << 8) | byte(8),
    };
}

[[nodiscard]] inline constexpr bool encode_http2_frame_header(std::span<char> output, std::uint32_t length,
    http2_frame_type type, std::uint8_t flags, std::uint32_t stream_id) noexcept {
    if (output.size() < http2_frame_header_bytes || length > http2_max_frame_size || stream_id > 0x7fffffffU) {
        return false;
    }
    output[0] = static_cast<char>((length >> 16) & 0xff);
    output[1] = static_cast<char>((length >> 8) & 0xff);
    output[2] = static_cast<char>(length & 0xff);
    output[3] = static_cast<char>(type);
    output[4] = static_cast<char>(flags);
    output[5] = static_cast<char>((stream_id >> 24) & 0x7f);
    output[6] = static_cast<char>((stream_id >> 16) & 0xff);
    output[7] = static_cast<char>((stream_id >> 8) & 0xff);
    output[8] = static_cast<char>(stream_id & 0xff);
    return true;
}

enum class http2_cleartext_preface_probe : std::uint8_t {
    http1,
    need_more_preface,
    complete_preface,
    drop_connection,
};

// Classify bytes that arrived on a cleartext listener before HTTP/1 parsing.
// A matching or truncated client preface is prior-knowledge HTTP/2. A `PRI `
// prefix that cannot become the preface is dropped. Anything else is HTTP/1.
[[nodiscard]] inline http2_cleartext_preface_probe probe_http2_cleartext_preface(
    std::string_view current) noexcept {
    if (current.empty()) {
        return http2_cleartext_preface_probe::http1;
    }

    if (current.starts_with(::ruvia::http2_client_preface) ||
        ::ruvia::http2_client_preface.starts_with(current)) {
        return current.size() >= ::ruvia::http2_client_preface.size()
                   ? http2_cleartext_preface_probe::complete_preface
                   : http2_cleartext_preface_probe::need_more_preface;
    }

    if (current.starts_with("PRI ")) {
        return http2_cleartext_preface_probe::drop_connection;
    }
    return http2_cleartext_preface_probe::http1;
}

}  // namespace ruvia

namespace ruvia {
class http2_connection;
namespace detail {
class http2_connection;
}  // namespace detail

enum class http2_finish_status : std::uint8_t {
    accepted,
    // The stream disappeared or was reset while its owner was finishing it.
    closed,
    // The submitted DATA does not reach the declared Content-Length, or the
    // trailer section cannot be sent. Nothing changed; the stream stays open.
    invalid_message,
    // An empty trailer section ends the stream with an empty DATA frame, which
    // nghttp2 sends only with positive connection and stream send windows
    // (RFC 9113 §6.9). Nothing changed; retry after the window opens.
    backpressured,
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

// received_settings 事件：已应用的非 ACK SETTINGS 中 nghttp2 解码后的条目。
// nghttp2 将重复出现的已知 ID 合并为一项（取最后的值，位置为首次出现处），
// 未知 ID 按线上顺序保留；若同一帧先降低后升高 HEADER_TABLE_SIZE，末尾另附一项
// 携带其中的最小值（RFC 7541 §4.2 要求编码器先确认该最小值）。超过
// max_frame_entries 项的 SETTINGS 帧本身即以 GOAWAY(ENHANCE_YOUR_CALM) 结束连接，
// 因此事件以定长数组内联保存，不分配内存，也不借用 feed() 输入。
class http2_received_settings final {
public:
    static constexpr std::size_t max_frame_entries = 32;
    static constexpr std::size_t max_entries = max_frame_entries + 1;

    http2_received_settings() = default;
    [[nodiscard]] std::size_t size() const noexcept {
        return size_;
    }
    // 越界抛 std::out_of_range。
    [[nodiscard]] http2_setting_entry at(std::size_t index) const;

private:
    friend class detail::http2_connection;
    std::array<http2_setting_entry, max_entries> entries_{};
    std::size_t size_{0};
};

// received_window_update 事件：连接实际应用的 WINDOW_UPDATE（stream 0 或存活流）。
struct http2_received_window_update final {
    std::uint32_t stream_id_{0};
    std::uint32_t increment_{0};
};

// received_priority 事件：带 PRIORITY 标志的 HEADERS 帧中 RFC 9113 §5.3.2 的原始
// dependency/weight（1..256）元数据，不代表调度策略。独立的 PRIORITY 帧
// （RFC 9113 §6.3 已弃用）不产生事件。
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
};

// feed() has all-or-nothing ownership for each supplied span: events_pending
// consumes nothing, every other result consumed the whole span. nghttp2 buffers
// an incomplete trailing frame itself, so accepted also covers partial input.
enum class http2_feed_result : std::uint8_t {
    events_pending,
    accepted,
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
// DATA flow-control progress is reported separately below.
enum class http2_submit_status : std::uint8_t {
    accepted,
    closed,
    invalid_state,
    invalid_message,
    peer_capability_unavailable,
};

enum class http2_data_submit_status : std::uint8_t {
    complete,
    backpressured,
    expectation_pending,
    closed,
    // The input would exceed the declared Content-Length, or end the stream
    // before reaching it. Nothing was consumed.
    invalid_message,
};

// Only consumed_bytes_ have been copied into serialized DATA. The unconsumed
// suffix always remains caller-owned. complete concerns this input, not socket I/O.
struct http2_data_submit_result final {
    std::size_t consumed_bytes_{0};
    http2_data_submit_status status_{http2_data_submit_status::complete};
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

}  // namespace ruvia
