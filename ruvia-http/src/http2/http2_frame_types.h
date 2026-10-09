#pragma once

#include <cstdint>
#include <string_view>

#include "ruvia/http/http2_framing.h"

namespace ruvia::detail {

using ruvia::http2_client_preface;
using ruvia::http2_default_max_frame_size;
using ruvia::http2_error_code;
using ruvia::http2_frame_header;
using ruvia::http2_frame_header_bytes;
using ruvia::http2_frame_type;
using ruvia::http2_setting_id;

inline constexpr std::uint32_t http2_max_frame_size_limit = http2_max_frame_size;
inline constexpr std::int32_t http2_default_initial_window_size = 65535;
inline constexpr std::uint8_t http2_flag_end_stream = 0x1;
inline constexpr std::uint8_t http2_flag_ack = 0x1;
inline constexpr std::uint8_t http2_flag_end_headers = 0x4;
inline constexpr std::uint8_t http2_flag_padded = 0x8;
inline constexpr std::uint8_t http2_flag_priority = 0x20;

}  // namespace ruvia::detail
