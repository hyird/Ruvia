#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <variant>

#include "ruvia/http/Http3VarInt.h"

namespace ruvia {
// RFC 9297 Capsule-Protocol is a Structured Field Boolean Item.
[[nodiscard]] std::variant<bool, Http3CodecError> parseHttpCapsuleProtocol(std::string_view value) noexcept;

inline constexpr std::uint64_t kHttpDatagramCapsuleType = 0;
inline constexpr std::uint64_t kHttp3DatagramErrorCode = 0x33;

struct Http3DatagramView final {
    std::uint64_t streamId{0};
    std::span<const char> payload{};
};
// RFC 9297: Quarter Stream ID prefixes a QUIC DATAGRAM payload. These codecs
// validate the stream-ID range and direction. The connection driver must also
// check both SETTINGS_H3_DATAGRAM values, QUIC DATAGRAM negotiation, and the
// request's datagram semantics/open directions before sending or delivering.
[[nodiscard]] std::variant<Http3DatagramView, Http3CodecError> decodeHttp3Datagram(std::span<const char> input) noexcept;
[[nodiscard]] std::variant<std::size_t, Http3CodecError> encodeHttp3DatagramPrefix(std::span<char> output, std::uint64_t streamId) noexcept;

struct HttpUdpDatagramView final {
    std::uint64_t contextId{0};
    std::span<const char> payload{};
};
// RFC 9298: Context ID zero carries a UDP payload; unknown contexts are dropped.
[[nodiscard]] std::variant<HttpUdpDatagramView, Http3CodecError> decodeHttpUdpDatagram(std::span<const char> input) noexcept;
[[nodiscard]] std::variant<std::size_t, Http3CodecError> encodeHttpUdpDatagramPrefix(std::span<char> output, std::uint64_t contextId = 0) noexcept;

struct HttpCapsuleEvent final {
    std::uint64_t type{0};
    std::span<const char> payload{};
    bool endCapsule{false};
};
using HttpCapsuleCallback = void (*)(void*, HttpCapsuleEvent);
enum class HttpCapsuleStatus : std::uint8_t { kNeedMoreData,
    kEnd,
    kTruncated,
    kLimit };
struct HttpCapsuleFeedResult final {
    HttpCapsuleStatus status{HttpCapsuleStatus::kNeedMoreData};
    std::size_t consumedBytes{};
    bool capsuleComplete{};
};
struct HttpCapsuleConfig final {
    std::uint64_t maxCapsuleLength{16 * 1024 * 1024};
};

// RFC 9297 incremental Capsule Protocol framing, shared by HTTP/1 tunnels and
// HTTP/2 or HTTP/3 Extended CONNECT streams. Header state is inline; payload
// fragments borrow feed input only during callbacks. Unknown types must be
// ignored by the extension consumer. No owned payload or event queue is created.
class HttpCapsuleDecoder final {
public:
    explicit HttpCapsuleDecoder(HttpCapsuleConfig config = {});
    [[nodiscard]] HttpCapsuleStatus feed(std::span<const char> input, bool fin, HttpCapsuleCallback callback, void* context);
    // Stops at the first complete capsule. The unconsumed suffix stays with the
    // caller; FIN is committed only after all input is consumed. This permits
    // pull-based adapters without buffering a queue of complete capsules.
    [[nodiscard]] HttpCapsuleFeedResult feedOne(std::span<const char> input, bool fin, HttpCapsuleCallback callback, void* context);

private:
    [[nodiscard]] HttpCapsuleFeedResult feedImpl(std::span<const char> input, bool fin, HttpCapsuleCallback callback, void* context, bool one);
    HttpCapsuleConfig config_;
    std::array<char, 16> header_{};
    std::size_t headerSize_{0}, typeSize_{0};
    std::uint64_t type_{0}, remaining_{0};
    bool payload_{false}, feeding_{false};
    HttpCapsuleStatus status_{HttpCapsuleStatus::kNeedMoreData};
};
[[nodiscard]] std::variant<std::size_t, Http3CodecError> encodeHttpCapsuleHeader(std::span<char> output, std::uint64_t type, std::uint64_t length) noexcept;
enum class HttpDatagramTransport : std::uint8_t { kCapsule,
    kQuic };
enum class HttpDatagramError : std::uint8_t {
    kNotNegotiated,
    kSendClosed,
    kPayloadTooLarge,
    kMalformed,
    kWrongStream,
};
struct HttpDatagramSessionConfig final {
    // Absent for HTTP/1 and HTTP/2. Required for QUIC DATAGRAM framing.
    std::optional<std::uint64_t> http3StreamId{};
    bool localH3Datagram{false};
    bool peerH3Datagram{false};
    bool quicDatagram{false};
    // Maximum QUIC DATAGRAM payload, after transport frame overhead is removed.
    std::size_t maxQuicPayloadBytes{0};
};
enum class Http3DatagramReceiveStatus : std::uint8_t { kDeliver,
    kDrop,
    kStreamError,
    kConnectionError };
struct Http3DatagramReceiveContext final {
    bool localH3Datagram{};
    bool streamExists{};
    bool receiveOpen{};
    bool supportsDatagrams{};
};
// RFC 9297: closed/uncreated streams drop packets; a known request without
// datagram semantics is aborted. SETTINGS arrival may race unreliable packets.
[[nodiscard]] Http3DatagramReceiveStatus planHttp3DatagramReceive(
    Http3DatagramView datagram, Http3DatagramReceiveContext context) noexcept;
struct HttpDatagramWritePlan final {
    std::array<char, 16> prefix{};
    std::size_t prefixSize{};
    std::span<const char> payload{};
    HttpDatagramTransport transport{HttpDatagramTransport::kCapsule};
};
struct HttpUdpDatagramWritePlan final {
    std::array<char, 24> prefix{};
    std::size_t prefixSize{0};
    std::span<const char> payload{};
    HttpDatagramTransport transport{HttpDatagramTransport::kCapsule};
};
// One accepted HTTP Datagram channel, with optional CONNECT-UDP semantics. Construct after request negotiation (clients
// may optimistically send before the response). Owns no memory or transport.
// Returned payloads borrow caller input; unknown context IDs are dropped.
class HttpDatagramSession final {
public:
    explicit HttpDatagramSession(HttpDatagramSessionConfig config = {});
    [[nodiscard]] bool quicDatagramsEnabled() const noexcept;
    void closeSend() noexcept {
        sendOpen_ = false;
    }
    void closeReceive() noexcept {
        receiveOpen_ = false;
    }
    [[nodiscard]] std::variant<HttpDatagramWritePlan, HttpDatagramError> prepareDatagram(
        std::span<const char> payload, HttpDatagramTransport transport) const noexcept;
    [[nodiscard]] std::variant<std::optional<std::span<const char>>, HttpDatagramError> receiveDatagram(
        std::span<const char> input, HttpDatagramTransport transport) const noexcept;
    [[nodiscard]] std::variant<HttpUdpDatagramWritePlan, HttpDatagramError> prepareUdpDatagram(
        std::span<const char> payload, HttpDatagramTransport transport) const noexcept;
    // Capsule input is the complete DATAGRAM capsule value, excluding type/length.
    // QUIC input includes Quarter Stream ID. Empty optional means silently dropped.
    [[nodiscard]] std::variant<std::optional<HttpUdpDatagramView>, HttpDatagramError> receiveUdpDatagram(
        std::span<const char> input, HttpDatagramTransport transport) const noexcept;

private:
    HttpDatagramSessionConfig config_;
    bool sendOpen_{true};
    bool receiveOpen_{true};
};
}  // namespace ruvia
