#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

#include "Http2FrameTypes.h"

namespace ruvia::detail {

class Http2StreamFlowControl final {
public:
    void setSendWindow(std::int32_t window) noexcept {
        sendWindow_ = window;
    }

    [[nodiscard]] std::int32_t sendWindow() const noexcept {
        return sendWindow_;
    }

    [[nodiscard]] bool addSendWindow(std::int64_t delta) noexcept {
        const auto updated = static_cast<std::int64_t>(sendWindow_) + delta;
        if (updated > std::numeric_limits<std::int32_t>::max() ||
            updated < std::numeric_limits<std::int32_t>::min()) {
            return false;
        }
        sendWindow_ = static_cast<std::int32_t>(updated);
        return true;
    }

    void consumeSend(std::size_t bytes) noexcept {
        sendWindow_ -= static_cast<std::int32_t>(bytes);
    }

    [[nodiscard]] bool consumeReceive(std::int32_t bytes) noexcept {
        if (bytes > receiveWindow_) {
            return false;
        }
        receiveWindow_ -= bytes;
        return true;
    }

    void restoreReceive(std::int32_t bytes) noexcept {
        receiveWindow_ += bytes;
    }

private:
    std::int32_t sendWindow_{kHttp2DefaultInitialWindowSize};
    std::int32_t receiveWindow_{static_cast<std::int32_t>(kHttp2LocalInitialWindowSize)};
};

}  // namespace ruvia::detail
