#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

#include "http2/http2_frame_codec.h"

namespace ruvia::detail {

inline constexpr std::size_t http2_window_update_frame_bytes = http2_frame_header_bytes + 4;

enum class http2_window_update_result : std::uint8_t { ok,
    zero_increment,
    overflow };

[[nodiscard]] inline std::uint32_t http2_window_update_increment(std::string_view payload_value) noexcept {
    return http2_read31(reinterpret_cast<const unsigned char*>(payload_value.data()));
}

[[nodiscard]] inline http2_window_update_result http2_apply_window_update(
    std::int32_t& window, std::uint32_t increment) noexcept {
    if (increment == 0) {
        return http2_window_update_result::zero_increment;
    }
    const auto amount = static_cast<std::int32_t>(increment);
    if (window > std::numeric_limits<std::int32_t>::max() - amount) {
        return http2_window_update_result::overflow;
    }
    window += amount;
    return http2_window_update_result::ok;
}

}  // namespace ruvia::detail
