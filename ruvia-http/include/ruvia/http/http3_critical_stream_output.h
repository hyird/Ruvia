#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "ruvia/http/Http3LocalCriticalStreams.h"
#include "ruvia/http/Http3VarInt.h"

namespace ruvia {

// One connection's locally initiated HTTP/3 control and QPACK stream prefixes,
// with one optional server GOAWAY suffix after control SETTINGS. This sans-I/O
// owner stores all output inline and retains bytes until accepted. The caller
// opens streams and drives transport retries; no stream prefix contains FIN.
// Keep this object alive at its stable address while an output span is borrowed.
class http3_critical_stream_output final {
public:
    enum class stream_kind : std::uint8_t { control,
        qpack_encoder,
        qpack_decoder };

    explicit http3_critical_stream_output(Http3LocalCriticalStreams prefixes) noexcept;
    http3_critical_stream_output(const http3_critical_stream_output&) = delete;
    http3_critical_stream_output& operator=(const http3_critical_stream_output&) = delete;
    http3_critical_stream_output(http3_critical_stream_output&&) = delete;
    http3_critical_stream_output& operator=(http3_critical_stream_output&&) = delete;

    // Backpressure is acknowledge(kind, 0); next(kind) then returns the
    // identical address, size and contents for the next transport attempt.
    [[nodiscard]] std::span<const char> next(stream_kind kind) & noexcept;
    std::span<const char> next(stream_kind) && = delete;
    [[nodiscard]] bool acknowledge(stream_kind kind, std::size_t accepted) noexcept;
    // Queues one server GOAWAY frame after the control-stream SETTINGS prefix.
    // The identifier must be a valid client-initiated request-stream boundary.
    [[nodiscard]] bool queue_goaway(std::uint64_t identifier) noexcept;
    [[nodiscard]] bool complete(stream_kind kind) const noexcept;
    [[nodiscard]] bool complete() const noexcept;

private:
    struct slot final {
        std::size_t consumed{};
        bool offered{};
        bool offering_goaway{};
    };

    [[nodiscard]] std::span<const char> prefix(stream_kind kind) const noexcept;
    [[nodiscard]] static std::size_t index(stream_kind kind) noexcept;

    Http3LocalCriticalStreams prefixes_;
    std::array<char, 3 * kHttp3VarIntMaxBytes> goaway_{};
    std::array<slot, 3> slots_{};
    std::size_t goaway_size_{};
    std::size_t goaway_consumed_{};
    bool goaway_queued_{};
};

}  // namespace ruvia
