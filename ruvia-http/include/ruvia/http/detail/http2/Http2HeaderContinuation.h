#pragma once

#include <cstdint>

#include "ruvia/http/detail/http2/Http2Frame.h"

namespace ruvia::detail {

enum class Http2HeaderBlockKind : std::uint8_t {
    kInitial,
    kTrailers,
    // The block must still be fully HPACK-decoded for connection-state
    // synchronization, but its HTTP fields do not mutate a live stream.
    kDiscarded
};

class Http2HeaderContinuation final {
public:
    [[nodiscard]] bool active() const noexcept {
        return streamId_ != 0;
    }

    [[nodiscard]] bool expectsFrameType(std::uint8_t frameType) const noexcept {
        return !active() || frameType == static_cast<std::uint8_t>(Http2FrameType::kContinuation);
    }

    [[nodiscard]] bool matches(std::uint32_t streamId) const noexcept {
        return streamId != 0 && streamId == streamId_;
    }

    void start(std::uint32_t streamId, Http2HeaderBlockKind kind) noexcept {
        streamId_ = streamId;
        kind_ = kind;
    }

    void reset() noexcept {
        streamId_ = 0;
        kind_ = Http2HeaderBlockKind::kInitial;
    }

    [[nodiscard]] Http2HeaderBlockKind kind() const noexcept {
        return kind_;
    }

    [[nodiscard]] Http2HeaderBlockKind finishKind() noexcept {
        const auto kind = kind_;
        reset();
        return kind;
    }

private:
    std::uint32_t streamId_{0};
    Http2HeaderBlockKind kind_{Http2HeaderBlockKind::kInitial};
};

}  // namespace ruvia::detail
