#pragma once

#include <array>
#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/http_status.h"

namespace ruvia {

namespace detail {
class hpack_decoder;
}  // namespace detail

// 32-bit HPACK dynamic-table byte capacity (RFC 7541 section 6.3). Checked
// before narrowing so negative or wider integers cannot wrap into valid sizes.
class hpack_table_size final {
public:
    constexpr hpack_table_size() noexcept = default;

    template <std::integral integer_type>
    constexpr hpack_table_size(integer_type value)
        : value_(is_valid_value(value)
                     ? static_cast<std::uint32_t>(value)
                     : throw std::invalid_argument("HPACK dynamic table size must be in 0..4294967295")) {}

    template <std::integral integer_type>
    [[nodiscard]] static constexpr bool is_valid_value(integer_type value) noexcept {
        return std::in_range<std::uint32_t>(+value);
    }

    template <std::integral integer_type>
    [[nodiscard]] static constexpr std::optional<hpack_table_size> try_from_value(integer_type value) noexcept {
        if (!is_valid_value(value)) {
            return std::nullopt;
        }
        return hpack_table_size(static_cast<std::uint32_t>(value), validated_tag_type{});
    }

    [[nodiscard]] constexpr std::uint32_t value() const noexcept {
        return value_;
    }

    friend constexpr auto operator<=>(hpack_table_size, hpack_table_size) = default;

private:
    struct validated_tag_type {};

    constexpr hpack_table_size(std::uint32_t value, validated_tag_type) noexcept
        : value_(value) {}

    std::uint32_t value_{0};
};

static_assert(std::is_trivially_copyable_v<hpack_table_size>);
static_assert(sizeof(hpack_table_size) == sizeof(std::uint32_t));

// nghttp2's inflater reports every malformed or truncated block as one
// decompression failure (RFC 7541 section 3.2 makes any of them a connection
// error), so the cause is not subdivided.
enum class hpack_decode_error : std::uint8_t {
    compression_error,
    callback_rejected,
};

// Stateful RFC 7541 decoder backed by nghttp2's HPACK inflater.
// decode() consumes one complete field block. Callback rejection stops further
// callbacks but still decodes the rest of the block, so the dynamic table stays
// synchronized and the decoder remains usable. A compression error or an
// allocation failure leaves the table out of step with the peer (RFC 7541 section
// 2.2 offers no recovery): the decoder is poisoned, every later decode() reports
// compression_error, and it must be discarded. Leading dynamic-table capacity
// updates are applied in wire order; updates after a field or above the
// configured maximum are compression errors.
// Lowering the maximum below the table's current capacity evicts at once, and
// the next block must then begin with a capacity update no larger than the
// lowest maximum set since (RFC 7541 section 4.2); a block without one is a
// compression error. Raising the maximum requires no update.
class hpack_decoder final {
public:
    hpack_decoder();
    ~hpack_decoder();
    hpack_decoder(const hpack_decoder&) = delete;
    hpack_decoder& operator=(const hpack_decoder&) = delete;
    hpack_decoder(hpack_decoder&&) noexcept;
    hpack_decoder& operator=(hpack_decoder&&) noexcept;

    // Only between decode() calls. During a header callback this throws
    // std::logic_error before changing either the allowance or the table.
    void set_max_dynamic_table_size(hpack_table_size bytes);

    // Header name/value views are borrowed only for the callback invocation.
    // Copy into owned storage when retaining a field beyond that invocation.
    // A callback must keep this decoder's implementation alive until decode()
    // returns. Nested decode() on the same implementation throws std::logic_error;
    // a different decoder may be used. Callback exceptions are rethrown after
    // consuming the block, so a valid block still synchronizes the dynamic table.
    template <typename callback>
        requires std::predicate<callback&, std::string_view, std::string_view>
    [[nodiscard]] std::optional<hpack_decode_error> decode(std::string_view block, callback&& callback_value) {
        // Always pass an object through the erased callback boundary. Taking
        // addressof(callback) directly would produce a function pointer when a
        // free function is supplied, which cannot be represented by void*.
        std::exception_ptr callback_failure;
        auto invocation = [&callback_value, &callback_failure](
                              std::string_view name, std::string_view value) {
            if (callback_failure) {
                return true;
            }
            try {
                return static_cast<bool>(std::invoke(callback_value, name, value));
            } catch (...) {
                callback_failure = std::current_exception();
                // Keep consuming the block so dynamic-table state remains
                // synchronized before the user's exception is rethrown.
                return true;
            }
        };
        using invocation_type = decltype(invocation);
        try {
            auto result_value = decode_with_callback(block, std::addressof(invocation),
                [](void* target, std::string_view name, std::string_view value) {
                    return std::invoke(*static_cast<invocation_type*>(target), name, value);
                });
            if (callback_failure) {
                std::rethrow_exception(callback_failure);
            }
            return result_value;
        } catch (...) {
            // A callback exception is the initiating failure; it still reaches
            // its owner when continuing the block failed as well.
            if (callback_failure) {
                std::rethrow_exception(callback_failure);
            }
            throw;
        }
    }

private:
    using header_callback_type = bool (*)(void*, std::string_view, std::string_view);
    [[nodiscard]] std::optional<hpack_decode_error> decode_with_callback(
        std::string_view block, void* target, header_callback_type callback);

    std::unique_ptr<detail::hpack_decoder> impl_;
};

// Field-level HPACK encoding through nghttp2's deflater with a zero-capacity
// dynamic table, so no call depends on or changes encoder state: an exact
// static-table match is indexed, any other field is a literal without indexing
// (never indexed for authorization, proxy-authorization, cookie and
// set-cookie, RFC 7541 section 7.1.3), Huffman-coded when that is shorter.
class hpack_encoder final {
public:
    // Names and values may borrow output. Aliasing inputs are owned before
    // appending can invalidate them. Allocation failure leaves the existing
    // output bytes unchanged.
    static void encode_header(
        std::string& output, std::string_view name, std::string_view value);
    static void encode_status(std::string& output, http_status_code status);
};

}  // namespace ruvia

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

namespace ruvia {

// A complete received frame, before its final payload fragment is processed.
// observation_sequence_ starts at zero after the preface and counts frame
// headers exposed by nghttp2, in wire order (including ACK, unknown extensions,
// PRIORITY and CONTINUATION). Frames discarded before nghttp2's begin-frame
// callback (for example DATA on an already reset stream) are not exposed or
// counted. Neither transport reads nor
// the preface are counted. Observation does not imply acceptance: protocol
// validation and mandatory rejection still run unchanged afterwards.
struct http2_frame_metadata final {
    std::uint64_t observation_sequence_{0};
    std::uint32_t payload_length_{0};
    std::uint32_t stream_id_{0};
    http2_frame_type type_{http2_frame_type::data};
    // Protocol flags exposed by nghttp2; undefined/reserved flag bits may have
    // been removed by that parser. Not a second interpretation of wire bytes.
    std::uint8_t flags_{0};
    // Raw SETTINGS entries in wire order, including unknown and duplicate IDs.
    // ACK SETTINGS have no entries. Oversize SETTINGS are still rejected by the
    // protocol core; settings_complete_ is false if the fixed observation budget
    // cannot represent the payload, or its size is not a multiple of six.
    std::array<http2_setting_entry, http2_received_settings::max_frame_entries> settings_{};
    std::size_t settings_count_{0};
    bool settings_complete_{true};
    std::optional<std::uint32_t> window_increment_{};
    // HEADERS with PRIORITY and standalone PRIORITY, never a scheduling policy.
    std::optional<http2_received_priority> priority_{};

    [[nodiscard]] std::span<const http2_setting_entry> settings() const& noexcept RUVIA_LIFETIMEBOUND {
        return {settings_.data(), settings_count_};
    }
    std::span<const http2_setting_entry> settings() const&& = delete;
};

enum class http2_header_observation_phase : std::uint8_t { begin,
    field,
    end };

// HPACK decoder facts, not reconstructed request fields. begin is delivered
// before HTTP field validation; field follows HPACK emission and precedes HTTP
// validation, including for a field that causes rejection. end means HPACK
// completed (not that HTTP semantics accepted the section). Rejected sections
// may have no end and nghttp2 may suppress their remaining field callbacks.
// HEADERS/PUSH_PROMISE starts a section; CONTINUATION does not start a new one.
// name_/value_ borrow decoder storage only for this callback. Empty on begin/end.
struct http2_header_metadata final {
    std::uint64_t first_frame_observation_sequence_{0};
    std::uint64_t frame_observation_sequence_{0};
    std::uint32_t stream_id_{0};
    std::size_t field_index_{0};
    http2_header_observation_phase phase_{http2_header_observation_phase::begin};
    std::string_view name_{};
    std::string_view value_{};
};

// Invoked synchronously inside feed(), never from next_event(). Observers must
// not mutate, re-enter, move or destroy this connection until feed() returns.
// The context is borrowed for the lifetime of the connection. nullptr disables
// its observer with no input retention, extra allocation or framing work.
using http2_frame_observer_type = void (*)(void*, const http2_frame_metadata&) noexcept;
using http2_header_observer_type = void (*)(void*, const http2_header_metadata&) noexcept;

}  // namespace ruvia
