#pragma once

#include <nghttp2/nghttp2.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "ruvia/http/http2_framing.h"
#include "ruvia/http/http_limits.h"

namespace ruvia::detail {

// RFC 9113 §5.1.1: stream identifiers are 31-bit values.
inline constexpr std::uint32_t http2_max_stream_id = 0x7fffffffU;

// One compile-time source of truth for every local receive constraint. These values
// drive both the advertised SETTINGS and the matching session options and fixed
// capacities; they are intentionally not runtime knobs that can diverge.
struct http2_local_settings final {
    static constexpr std::uint32_t header_table_size = 4096;
    static constexpr std::uint32_t max_concurrent_streams = 128;
    static constexpr std::uint32_t initial_window_size = 1024 * 1024;
    static constexpr std::uint32_t max_frame_size = http2_default_max_frame_size;
    static constexpr std::uint32_t max_header_list_size =
        static_cast<std::uint32_t>(max_http_header_bytes);
    static constexpr std::uint32_t enable_connect_protocol = 1;
};

static_assert(http2_local_settings::initial_window_size <=
              static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()));

// The local SETTINGS frame of the connection preface. Only a client may
// advertise ENABLE_PUSH=1 (RFC 9113 §6.5.2).
[[nodiscard]] inline std::array<nghttp2_settings_entry, 8> http2_local_settings_entries(bool enable_push) noexcept {
    return {{
        {NGHTTP2_SETTINGS_HEADER_TABLE_SIZE, http2_local_settings::header_table_size},
        {NGHTTP2_SETTINGS_ENABLE_PUSH, enable_push ? 1U : 0U},
        {NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, http2_local_settings::max_concurrent_streams},
        {NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, http2_local_settings::initial_window_size},
        {NGHTTP2_SETTINGS_MAX_FRAME_SIZE, http2_local_settings::max_frame_size},
        {NGHTTP2_SETTINGS_MAX_HEADER_LIST_SIZE, http2_local_settings::max_header_list_size},
        {NGHTTP2_SETTINGS_ENABLE_CONNECT_PROTOCOL, http2_local_settings::enable_connect_protocol},
        {NGHTTP2_SETTINGS_NO_RFC7540_PRIORITIES, 1},
    }};
}

}  // namespace ruvia::detail
