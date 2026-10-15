#pragma once

#include <cstdint>
#include <utility>

#include "http2/http2_local_settings.h"

namespace ruvia::detail {

// Consumed receive credit is intentionally not advertised frame-by-frame. Waiting
// until half the configured window has accumulated preserves at least half-window
// forward progress while bounding WINDOW_UPDATE amplification for tiny DATA frames.
inline constexpr std::uint32_t http2_receive_window_update_threshold =
    http2_local_settings::initial_window_size / 2;

static_assert(http2_receive_window_update_threshold > 0);

class http2_receive_window_credit final {
public:
    void add(std::uint32_t bytes_value) noexcept {
        pending_ += bytes_value;
    }

    [[nodiscard]] bool ready() const noexcept {
        return pending_ >= http2_receive_window_update_threshold;
    }

    [[nodiscard]] bool ready_after(std::uint32_t bytes_value) const noexcept {
        return ready() || bytes_value >= http2_receive_window_update_threshold - pending_;
    }

    [[nodiscard]] std::uint32_t take() noexcept {
        return std::exchange(pending_, 0);
    }

private:
    std::uint32_t pending_{0};
};

}  // namespace ruvia::detail
