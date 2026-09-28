#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "ruvia/http/Http3LocalCriticalStreams.h"
#include "ruvia/http/Http3VarInt.h"

namespace ruvia::detail {

// One connection's locally initiated HTTP/3 control and QPACK stream prefixes,
// with one optional GOAWAY suffix after control SETTINGS. QUIC stream IDs/opening
// and SSL retry are owned by the transport driver. This address-stable owner
// retains bytes until accepted; no stream prefix contains FIN.
class Http3CriticalStreamOutput final {
public:
    enum class Kind : std::uint8_t { kControl,
        kQpackEncoder,
        kQpackDecoder };

    explicit Http3CriticalStreamOutput(Http3LocalCriticalStreams prefixes) noexcept;
    Http3CriticalStreamOutput(const Http3CriticalStreamOutput&) = delete;
    Http3CriticalStreamOutput& operator=(const Http3CriticalStreamOutput&) = delete;
    Http3CriticalStreamOutput(Http3CriticalStreamOutput&&) = delete;
    Http3CriticalStreamOutput& operator=(Http3CriticalStreamOutput&&) = delete;

    // A WANT result is acknowledge(kind, 0); next(kind) then returns the
    // identical address, size and contents required by SSL_write_ex retry.
    [[nodiscard]] std::span<const char> next(Kind kind) & noexcept;
    std::span<const char> next(Kind) && = delete;
    [[nodiscard]] bool acknowledge(Kind kind, std::size_t accepted) noexcept;
    // Queues one server GOAWAY frame after the control-stream SETTINGS prefix.
    // The identifier must be a valid client-initiated request-stream boundary.
    [[nodiscard]] bool queueGoaway(std::uint64_t identifier) noexcept;
    [[nodiscard]] bool complete(Kind kind) const noexcept;
    [[nodiscard]] bool complete() const noexcept;

private:
    struct Slot final {
        std::size_t consumed{};
        bool offered{};
        bool offeringGoaway{};
    };

    [[nodiscard]] std::span<const char> prefix(Kind kind) const noexcept;
    [[nodiscard]] static std::size_t index(Kind kind) noexcept;

    Http3LocalCriticalStreams prefixes_;
    std::array<char, 3 * kHttp3VarIntMaxBytes> goaway_{};
    std::array<Slot, 3> slots_{};
    std::size_t goawaySize_{};
    std::size_t goawayConsumed_{};
    bool goawayQueued_{};
};

}  // namespace ruvia::detail
