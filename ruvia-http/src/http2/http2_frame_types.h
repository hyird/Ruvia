#pragma once

#include <cstdint>

#include "ruvia/http/http2_framing.h"

namespace ruvia::detail {

inline constexpr std::int32_t http2_default_initial_window_size = 65535;
// RFC 9113 §5.1.1: stream identifiers and window increments are 31-bit values.
inline constexpr std::uint32_t http2_max_stream_id = 0x7fffffffU;
inline constexpr std::uint8_t http2_flag_end_stream = 0x1;
inline constexpr std::uint8_t http2_flag_ack = 0x1;
inline constexpr std::uint8_t http2_flag_end_headers = 0x4;
inline constexpr std::uint8_t http2_flag_padded = 0x8;
inline constexpr std::uint8_t http2_flag_priority = 0x20;

}  // namespace ruvia::detail
