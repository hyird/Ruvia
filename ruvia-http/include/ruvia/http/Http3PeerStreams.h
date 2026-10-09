#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <unordered_map>
#include <variant>

#include "ruvia/http/Http3VarInt.h"

namespace ruvia {

enum class Http3PeerRole : std::uint8_t {
    kClient,
    kServer,
};

enum class Http3StreamIdType : std::uint8_t {
    kClientBidirectional,
    kServerBidirectional,
    kClientUnidirectional,
    kServerUnidirectional,
};

// Classifies the RFC 9000 stream-ID initiator/direction bits and rejects
// values outside QUIC's 62-bit stream-ID range.
[[nodiscard]] inline constexpr std::optional<Http3StreamIdType> http3StreamIdType(
    std::uint64_t streamId) noexcept {
    if (streamId > kHttp3VarIntMax) {
        return std::nullopt;
    }
    switch (streamId & 3U) {
        case 0:
            return Http3StreamIdType::kClientBidirectional;
        case 1:
            return Http3StreamIdType::kServerBidirectional;
        case 2:
            return Http3StreamIdType::kClientUnidirectional;
        case 3:
            return Http3StreamIdType::kServerUnidirectional;
    }
    return std::nullopt;
}

[[nodiscard]] inline constexpr bool isHttp3RequestStreamId(std::uint64_t streamId) noexcept {
    return http3StreamIdType(streamId) == Http3StreamIdType::kClientBidirectional;
}

[[nodiscard]] inline constexpr bool isHttp3UnidirectionalStreamId(std::uint64_t streamId) noexcept {
    const auto type = http3StreamIdType(streamId);
    return type == Http3StreamIdType::kClientUnidirectional ||
           type == Http3StreamIdType::kServerUnidirectional;
}

[[nodiscard]] inline constexpr bool isHttp3ClientUnidirectionalStreamId(
    std::uint64_t streamId) noexcept {
    return http3StreamIdType(streamId) == Http3StreamIdType::kClientUnidirectional;
}

[[nodiscard]] inline constexpr bool isHttp3PeerUnidirectionalStreamId(
    Http3PeerRole localRole, std::uint64_t streamId) noexcept {
    return localRole == Http3PeerRole::kClient
               ? http3StreamIdType(streamId) == Http3StreamIdType::kServerUnidirectional
               : isHttp3ClientUnidirectionalStreamId(streamId);
}

[[nodiscard]] inline constexpr bool isHttp3PeerBidirectionalStreamId(
    Http3PeerRole localRole, std::uint64_t streamId) noexcept {
    // HTTP/3 clients do not accept server-initiated bidirectional streams.
    return localRole == Http3PeerRole::kServer && isHttp3RequestStreamId(streamId);
}

enum class Http3PeerStreamKind : std::uint8_t {
    kUnclassified,
    kControl,
    kPush,
    kQpackEncoder,
    kQpackDecoder,
    kUnknown,
};
enum class Http3PeerStreamError : std::uint8_t {
    kStreamCreationError,
    kClosedCriticalStream,
    kExcessiveLoad,
};

struct Http3PeerStreamFeed final {
    Http3PeerStreamKind kind{Http3PeerStreamKind::kUnclassified};
    std::uint64_t streamType{0};
    // Number of input bytes consumed by the classifier. `remaining` borrows the
    // unconsumed bytes from this call; the caller owns their subsequent parsing.
    std::size_t consumed{0};
    std::span<const char> remaining{};
    bool fin{false};
    bool reset{false};
    bool closed{false};
};

struct Http3PeerStreamLimits final {
    std::size_t maxActiveStreams{128};
};

// Connection-local classifier for streams initiated by the QUIC peer. It owns
// only partial type-varint state; payload bytes are always borrowed by the caller.
class Http3PeerStreams final {
public:
    explicit Http3PeerStreams(Http3PeerRole localRole, std::pmr::memory_resource* resource,
        Http3PeerStreamLimits limits = {});

    [[nodiscard]] std::variant<Http3PeerStreamFeed, Http3PeerStreamError> feed(
        std::uint64_t streamId, std::span<const char> bytes, bool fin = false,
        bool reset = false);

    // Validate a peer-initiated bidirectional stream. HTTP/3 servers accept
    // client request streams; clients reject server-initiated bidi streams.
    [[nodiscard]] static std::variant<std::monostate, Http3PeerStreamError> acceptBidirectional(
        Http3PeerRole localRole, std::uint64_t streamId) noexcept;

    // Called only after transport retirement; critical streams cannot retire
    // independently of the connection.
    [[nodiscard]] bool retireNonCriticalStream(std::uint64_t streamId) noexcept;
    [[nodiscard]] std::size_t activeStreamCount() const noexcept;

private:
    struct State final {
        std::array<char, 8> typeBytes{};
        std::size_t typeSize{0};
        Http3PeerStreamKind kind{Http3PeerStreamKind::kUnclassified};
        std::uint64_t streamType{0};
    };

    [[nodiscard]] std::variant<std::monostate, Http3PeerStreamError> validatePeerUni(
        std::uint64_t streamId) const noexcept;
    [[nodiscard]] static bool isCritical(Http3PeerStreamKind kind) noexcept;

    Http3PeerRole localRole_;
    Http3PeerStreamLimits limits_;
    std::pmr::unordered_map<std::uint64_t, State> streams_;
    bool controlSeen_{false};
    bool encoderSeen_{false};
    bool decoderSeen_{false};
};

}  // namespace ruvia
