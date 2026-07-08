#pragma once

#include <cstdint>

#include "Http2Frame.h"

namespace ruvia::detail {

// NOTE: http2ReadFramePhase (the ConnectionScanner phase mapping for frame reads)
// lives in ruvia-web/src/net/http2/Http2FramePhase.h -- ConnectionScanner is
// asio-bound I/O policy, and this header is part of the asio-free protocol core.

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

    void start(std::uint32_t streamId, bool trailers) noexcept {
        streamId_ = streamId;
        trailers_ = trailers;
    }

    void reset() noexcept {
        streamId_ = 0;
        trailers_ = false;
    }

    [[nodiscard]] bool finishWasTrailers() noexcept {
        const bool trailers = trailers_;
        reset();
        return trailers;
    }

private:
    std::uint32_t streamId_{0};
    bool trailers_{false};
};

}  // namespace ruvia::detail
