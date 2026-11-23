#pragma once

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <variant>

#include "borrowed_text.h"
#include "byte_view.h"
#include "decode_step.h"
#include "quic_varint.h"

namespace ruvia {

using http_capsule_codec_error = quic_varint_error;

// RFC 9297 Capsule-Protocol is a Structured Field Boolean Item.
[[nodiscard]] std::optional<bool> parse_http_capsule_protocol(std::string_view value) noexcept;

inline constexpr std::uint64_t http_datagram_capsule_type = 0;
struct http_udp_datagram_view final {
    std::uint64_t context_id_{0};
    byte_view payload_{};
};
// RFC 9298: context ID zero carries a UDP payload; unknown contexts are dropped.
// The payload view borrows `input`.
[[nodiscard]] std::optional<http_udp_datagram_view> decode_http_udp_datagram(byte_view input RUVIA_LIFETIMEBOUND) noexcept;
template <detail::http_temporary_owning_range<std::byte> input_type>
std::optional<http_udp_datagram_view> decode_http_udp_datagram(input_type&&) noexcept = delete;
template <typename element_type>
    requires std::same_as<std::remove_cv_t<element_type>, std::byte>
std::optional<http_udp_datagram_view> decode_http_udp_datagram(std::initializer_list<element_type>) noexcept = delete;
[[nodiscard]] std::variant<std::size_t, http_capsule_codec_error> encode_http_udp_datagram_prefix(mutable_byte_view output, std::uint64_t context_id = 0) noexcept;

struct http_capsule_event final {
    std::uint64_t type_{0};
    byte_view payload_{};
    bool end_capsule_{false};
};
using http_capsule_callback_type = void (*)(void*, http_capsule_event);
enum class http_capsule_decode_error : std::uint8_t {
    // The stream ended inside a capsule header or payload.
    truncated,
    // A kept capsule exceeds http_capsule_options::max_capsule_bytes_.
    limit_exceeded,
};
using http_capsule_decode_failure = decode_failure<http_capsule_decode_error>;
// A kept capsule ended within the input feed_one() consumed.
struct http_capsule_boundary final {};
// One feed() or feed_one() outcome: more input needed, a kept capsule ended
// (feed_one() only), the stream ended at FIN, or a failure.
using http_capsule_feed_result = decode_step<decode_need_input, http_capsule_boundary, decode_complete, http_capsule_decode_failure>;
// max_capsule_bytes_ bounds the capsules a reader keeps (see
// http_capsule_retention); dropped capsules are skipped at any length.
struct http_capsule_options final {
    std::uint64_t max_capsule_bytes_{16 * 1024 * 1024};
};
// Which capsules a reader keeps. RFC 9297 section 3.2: a recipient silently
// drops a capsule it does not use and skips over it. Dropped capsules produce
// no callback, are not buffered and are not subject to max_capsule_bytes_.
enum class http_capsule_retention : std::uint8_t {
    // Every capsule: a generic Capsule Protocol reader handles unknown types.
    all,
    // DATAGRAM capsules (RFC 9297 section 3.5) only.
    datagrams,
    // DATAGRAM capsules carrying RFC 9298 Context ID 0 (UDP payloads); other
    // Context IDs are unknown and dropped. The Context ID is inspected before
    // the length limit applies. A DATAGRAM too short for its Context ID is kept
    // so that the reader rejects it as malformed.
    udp_payloads,
};

// RFC 9297 incremental Capsule Protocol framing for HTTP/1 tunnels. Header state is inline; payload
// fragments borrow feed input (or, for a kept CONNECT-UDP Context ID, decoder
// storage) only during callbacks. No owned payload or event queue is created.
// The retention of a capsule is decided from the feed call that completes its
// header (and, for udp_payloads, its Context ID).
// FIN finishes the decoder and a failure fails it; either then repeats its
// result without consuming input until reset().
class http_capsule_decoder final {
public:
    explicit http_capsule_decoder(http_capsule_options config = {});
    [[nodiscard]] decoder_state state() const noexcept {
        return state_;
    }
    // Starts a new capsule stream with the constructed configuration.
    void reset() noexcept;
    // Consumes all of input unless a kept capsule exceeds the length limit.
    [[nodiscard]] http_capsule_feed_result feed(byte_view input, bool fin, http_capsule_callback_type callback, void* context, http_capsule_retention retention = http_capsule_retention::all) &;
    http_capsule_feed_result feed(byte_view, bool, http_capsule_callback_type, void*, http_capsule_retention = http_capsule_retention::all) && = delete;
    template <typename visitor_type>
        requires std::invocable<visitor_type&, http_capsule_event>
    [[nodiscard]] http_capsule_feed_result feed(byte_view input, bool fin, visitor_type&& visitor, http_capsule_retention retention = http_capsule_retention::all) & {
        auto& target = visitor;
        return feed(input, fin, +[](void* context, http_capsule_event event) {
            (*static_cast<std::remove_reference_t<visitor_type>*>(context))(event);
        }, std::addressof(target), retention);
    }
    template <typename visitor_type>
        requires std::invocable<visitor_type&, http_capsule_event>
    http_capsule_feed_result feed(byte_view, bool, visitor_type&&, http_capsule_retention = http_capsule_retention::all) && = delete;
    // Stops at the first complete kept capsule. The unconsumed suffix stays with
    // the caller; FIN is committed only after all input is consumed, so a
    // capsule that ends exactly at FIN reports the boundary and leaves the
    // decoder finished. This permits pull-based adapters without buffering a
    // queue of complete capsules.
    [[nodiscard]] http_capsule_feed_result feed_one(byte_view input, bool fin, http_capsule_callback_type callback, void* context, http_capsule_retention retention = http_capsule_retention::all) &;
    http_capsule_feed_result feed_one(byte_view, bool, http_capsule_callback_type, void*, http_capsule_retention = http_capsule_retention::all) && = delete;
    template <typename visitor_type>
        requires std::invocable<visitor_type&, http_capsule_event>
    [[nodiscard]] http_capsule_feed_result feed_one(byte_view input, bool fin, visitor_type&& visitor, http_capsule_retention retention = http_capsule_retention::all) & {
        auto& target = visitor;
        return feed_one(input, fin, +[](void* context, http_capsule_event event) {
            (*static_cast<std::remove_reference_t<visitor_type>*>(context))(event);
        }, std::addressof(target), retention);
    }
    template <typename visitor_type>
        requires std::invocable<visitor_type&, http_capsule_event>
    http_capsule_feed_result feed_one(byte_view, bool, visitor_type&&, http_capsule_retention = http_capsule_retention::all) && = delete;

private:
    enum class payload_mode : std::uint8_t { deliver,
        skip,
        context_id };
    [[nodiscard]] http_capsule_feed_result feed_impl(byte_view input, bool fin, http_capsule_callback_type callback, void* context, bool one, http_capsule_retention retention);
    http_capsule_options config_;
    std::array<std::byte, 16> header_{};
    std::array<std::byte, 8> context_id_{};
    std::size_t header_size_{0}, type_size_{0}, context_id_size_{0};
    std::uint64_t type_{0}, remaining_{0};
    payload_mode mode_{payload_mode::deliver};
    bool payload_{false}, feeding_{false};
    decoder_state state_{decoder_state::active};
    // Meaningful only while state_ is failed.
    http_capsule_decode_error error_{http_capsule_decode_error::truncated};
};
[[nodiscard]] std::variant<std::size_t, http_capsule_codec_error> encode_http_capsule_header(mutable_byte_view output, std::uint64_t type, std::uint64_t length) noexcept;
enum class http_datagram_error : std::uint8_t {
    send_closed,
    payload_too_large,
    malformed,
};
// A valid output snapshot produced by http_datagram_session. The inline prefix
// belongs to this plan; payload() continues to borrow the caller's input.
class http_datagram_write_plan final {
public:
    [[nodiscard]] constexpr byte_view prefix() const& noexcept RUVIA_LIFETIMEBOUND {
        return byte_view(prefix_.data(), prefix_size_);
    }
    byte_view prefix() const&& = delete;

    [[nodiscard]] constexpr byte_view payload() const noexcept {
        return payload_;
    }

private:
    friend class http_datagram_session;
    http_datagram_write_plan() noexcept = default;

    std::array<std::byte, 24> prefix_{};
    std::size_t prefix_size_{0};
    byte_view payload_{};
};
// What one DATAGRAM capsule value carries. `raw` is the HTTP Datagram payload
// as is, including any protocol-specific Context ID. `connect_udp` is one UDP
// packet of an RFC 9298 tunnel: sending prefixes Context ID 0 and bounds the
// payload at 65527 bytes; receiving strips Context ID 0 and drops every other
// Context ID.
enum class http_datagram_mode : std::uint8_t {
    raw,
    connect_udp,
};
// One accepted HTTP Datagram channel. Construct after request negotiation (clients
// may optimistically send before the response). Owns no memory or transport.
// Returned payloads borrow caller input.
class http_datagram_session final {
public:
    http_datagram_session() noexcept = default;
    void close_send() noexcept {
        send_open_ = false;
    }
    void close_receive() noexcept {
        receive_open_ = false;
    }
    [[nodiscard]] std::variant<http_datagram_write_plan, http_datagram_error> prepare_datagram(http_datagram_mode mode, byte_view payload RUVIA_LIFETIMEBOUND) const noexcept;
    template <detail::http_temporary_owning_range<std::byte> payload_type>
    std::variant<http_datagram_write_plan, http_datagram_error> prepare_datagram(http_datagram_mode, payload_type&&) const noexcept = delete;
    template <typename element_type>
        requires std::same_as<std::remove_cv_t<element_type>, std::byte>
    std::variant<http_datagram_write_plan, http_datagram_error> prepare_datagram(http_datagram_mode, std::initializer_list<element_type>) const noexcept = delete;
    // Capsule input is the complete DATAGRAM capsule value, excluding type/length.
    // Returns the payload within `input`. Empty optional means silently dropped:
    // the receive side is closed, or `connect_udp` saw an unknown Context ID.
    // Only `connect_udp` reports errors (malformed Context ID, oversized payload).
    [[nodiscard]] std::variant<std::optional<byte_view>, http_datagram_error> receive_datagram(http_datagram_mode mode, byte_view input RUVIA_LIFETIMEBOUND) const noexcept;
    template <detail::http_temporary_owning_range<std::byte> input_type>
    std::variant<std::optional<byte_view>, http_datagram_error> receive_datagram(http_datagram_mode, input_type&&) const noexcept = delete;
    template <typename element_type>
        requires std::same_as<std::remove_cv_t<element_type>, std::byte>
    std::variant<std::optional<byte_view>, http_datagram_error> receive_datagram(http_datagram_mode, std::initializer_list<element_type>) const noexcept = delete;

private:
    [[nodiscard]] static std::variant<http_datagram_write_plan, http_datagram_error> plan_datagram_capsule(byte_view payload RUVIA_LIFETIMEBOUND, byte_view context_prefix) noexcept;

    bool send_open_{true};
    bool receive_open_{true};
};

}  // namespace ruvia
