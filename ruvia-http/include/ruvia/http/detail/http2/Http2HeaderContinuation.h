#pragma once

#include <cstdint>

#include "ruvia/http/detail/http2/Http2Frame.h"

namespace ruvia::detail {

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
