#pragma once

#include <cstdint>
#include <utility>

#include "ruvia/http/detail/http2/settings/Http2LocalSettings.h"

namespace ruvia::detail {

// Consumed receive credit is intentionally not advertised frame-by-frame. Waiting
// until half the configured window has accumulated preserves at least half-window
// forward progress while bounding WINDOW_UPDATE amplification for tiny DATA frames.
inline constexpr std::uint32_t kHttp2ReceiveWindowUpdateThreshold = Http2LocalSettings::kInitialWindowSize / 2;

static_assert(kHttp2ReceiveWindowUpdateThreshold > 0);

class Http2ReceiveWindowCredit final {
public:
    void add(std::uint32_t bytes) noexcept {
        pending_ += bytes;
    }

    [[nodiscard]] bool ready() const noexcept {
        return pending_ >= kHttp2ReceiveWindowUpdateThreshold;
    }

    [[nodiscard]] std::uint32_t take() noexcept {
        return std::exchange(pending_, 0);
    }

private:
    std::uint32_t pending_{0};
};

}  // namespace ruvia::detail
