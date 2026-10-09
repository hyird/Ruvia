#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <variant>

#include "ruvia/http/http3_var_int.h"

namespace ruvia {
// RFC 9297 Capsule-Protocol is a Structured Field Boolean Item.
[[nodiscard]] std::variant<bool, http3_codec_error> parse_http_capsule_protocol(std::string_view value) noexcept;

inline constexpr std::uint64_t http_datagram_capsule_type = 0;
inline constexpr std::uint64_t http3_datagram_error_code = 0x33;

struct http3_datagram_view final {
    std::uint64_t stream_id_{0};
    std::span<const char> payload_{};
};
// RFC 9297: Quarter Stream ID prefixes a QUIC DATAGRAM payload. These codecs
// validate the stream-ID range and direction. The connection driver must also
// check both SETTINGS_H3_DATAGRAM values, QUIC DATAGRAM negotiation, and the
// request's datagram semantics/open directions before sending or delivering.
[[nodiscard]] std::variant<http3_datagram_view, http3_codec_error> decode_http3_datagram(std::span<const char> input) noexcept;
[[nodiscard]] std::variant<std::size_t, http3_codec_error> encode_http3_datagram_prefix(std::span<char> output, std::uint64_t stream_id) noexcept;

struct http_udp_datagram_view final {
    std::uint64_t context_id_{0};
    std::span<const char> payload_{};
};
// RFC 9298: context ID zero carries a UDP payload; unknown contexts are dropped.
[[nodiscard]] std::variant<http_udp_datagram_view, http3_codec_error> decode_http_udp_datagram(std::span<const char> input) noexcept;
[[nodiscard]] std::variant<std::size_t, http3_codec_error> encode_http_udp_datagram_prefix(std::span<char> output, std::uint64_t context_id = 0) noexcept;

struct http_capsule_event final {
    std::uint64_t type_{0};
    std::span<const char> payload_{};
    bool end_capsule_{false};
};
using http_capsule_callback_type = void (*)(void*, http_capsule_event);
enum class http_capsule_status : std::uint8_t { need_more_data,
    end,
    truncated,
    limit };
struct http_capsule_feed_result final {
    http_capsule_status status_{http_capsule_status::need_more_data};
    std::size_t consumed_bytes_{};
    bool capsule_complete_{};
};
struct http_capsule_config final {
    std::uint64_t max_capsule_length_{16 * 1024 * 1024};
};

// RFC 9297 incremental Capsule Protocol framing, shared by HTTP/1 tunnels and
// HTTP/2 or HTTP/3 Extended CONNECT streams. Header state is inline; payload
// fragments borrow feed input only during callbacks. Unknown types must be
// ignored by the extension consumer. No owned payload or event queue is created.
class http_capsule_decoder final {
public:
    explicit http_capsule_decoder(http_capsule_config config = {});
    [[nodiscard]] http_capsule_status feed(std::span<const char> input, bool fin, http_capsule_callback_type callback, void* context);
    // Stops at the first complete capsule. The unconsumed suffix stays with the
    // caller; FIN is committed only after all input is consumed. This permits
    // pull-based adapters without buffering a queue of complete capsules.
    [[nodiscard]] http_capsule_feed_result feed_one(std::span<const char> input, bool fin, http_capsule_callback_type callback, void* context);

private:
    [[nodiscard]] http_capsule_feed_result feed_impl(std::span<const char> input, bool fin, http_capsule_callback_type callback, void* context, bool one);
    http_capsule_config config_;
    std::array<char, 16> header_{};
    std::size_t header_size_{0}, type_size_{0};
    std::uint64_t type_{0}, remaining_{0};
    bool payload_{false}, feeding_{false};
    http_capsule_status status_{http_capsule_status::need_more_data};
};
[[nodiscard]] std::variant<std::size_t, http3_codec_error> encode_http_capsule_header(std::span<char> output, std::uint64_t type, std::uint64_t length) noexcept;
enum class http_datagram_transport : std::uint8_t { capsule,
    quic };
enum class http_datagram_error : std::uint8_t {
    not_negotiated,
    send_closed,
    payload_too_large,
    malformed,
    wrong_stream,
};
struct http_datagram_session_config final {
    // Absent for HTTP/1 and HTTP/2. Required for QUIC DATAGRAM framing.
    std::optional<std::uint64_t> http3_stream_id_{};
    bool local_h3_datagram_{false};
    bool peer_h3_datagram_{false};
    bool quic_datagram_{false};
    // Maximum QUIC DATAGRAM payload, after transport frame overhead is removed.
    std::size_t max_quic_payload_bytes_{0};
};
enum class http3_datagram_receive_status : std::uint8_t { deliver,
    drop,
    stream_error,
    connection_error };
struct http3_datagram_receive_context final {
    bool local_h3_datagram_{};
    bool stream_exists_{};
    bool receive_open_{};
    bool supports_datagrams_{};
};
// RFC 9297: closed/uncreated streams drop packets; a known request without
// datagram semantics is aborted. SETTINGS arrival may race unreliable packets.
[[nodiscard]] http3_datagram_receive_status plan_http3_datagram_receive(
    http3_datagram_view datagram, http3_datagram_receive_context context_value) noexcept;
struct http_datagram_write_plan final {
    std::array<char, 16> prefix_{};
    std::size_t prefix_size_{};
    std::span<const char> payload_{};
    http_datagram_transport transport_{http_datagram_transport::capsule};
};
struct http_udp_datagram_write_plan final {
    std::array<char, 24> prefix_{};
    std::size_t prefix_size_{0};
    std::span<const char> payload_{};
    http_datagram_transport transport_{http_datagram_transport::capsule};
};
// One accepted HTTP Datagram channel, with optional CONNECT-UDP semantics. Construct after request negotiation (clients
// may optimistically send before the response). Owns no memory or transport.
// Returned payloads borrow caller input; unknown context IDs are dropped.
class http_datagram_session final {
public:
    explicit http_datagram_session(http_datagram_session_config config = {});
    [[nodiscard]] bool quic_datagrams_enabled() const noexcept;
    void close_send() noexcept {
        send_open_ = false;
    }
    void close_receive() noexcept {
        receive_open_ = false;
    }
    [[nodiscard]] std::variant<http_datagram_write_plan, http_datagram_error> prepare_datagram(
        std::span<const char> payload, http_datagram_transport transport) const noexcept;
    [[nodiscard]] std::variant<std::optional<std::span<const char>>, http_datagram_error> receive_datagram(
        std::span<const char> input, http_datagram_transport transport) const noexcept;
    [[nodiscard]] std::variant<http_udp_datagram_write_plan, http_datagram_error> prepare_udp_datagram(
        std::span<const char> payload, http_datagram_transport transport) const noexcept;
    // Capsule input is the complete DATAGRAM capsule value, excluding type/length.
    // QUIC input includes Quarter Stream ID. Empty optional means silently dropped.
    [[nodiscard]] std::variant<std::optional<http_udp_datagram_view>, http_datagram_error> receive_udp_datagram(
        std::span<const char> input, http_datagram_transport transport) const noexcept;

private:
    http_datagram_session_config config_;
    bool send_open_{true};
    bool receive_open_{true};
};
}  // namespace ruvia
