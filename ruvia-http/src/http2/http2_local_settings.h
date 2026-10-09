#pragma once

#include <cstddef>
#include <cstdint>

#include "ruvia/http/http_limits.h"

#include "http2/http2_frame_codec.h"

namespace ruvia::detail {

// One compile-time source of truth for every local receive constraint. These values
// drive both the advertised SETTINGS bytes and the matching in-memory accounting /
// fixed capacities; they are intentionally not runtime knobs that can diverge.
struct http2_local_settings final {
    static constexpr std::uint32_t header_table_size = 4096;
    static constexpr std::uint32_t enable_push = 0;
    static constexpr std::uint32_t max_concurrent_streams = 128;
    static constexpr std::uint32_t initial_window_size = 1024 * 1024;
    static constexpr std::uint32_t max_frame_size = http2_default_max_frame_size;
    static constexpr std::uint32_t max_header_list_size =
        static_cast<std::uint32_t>(max_http_header_bytes);
    static constexpr std::uint32_t enable_connect_protocol = 1;
    static constexpr std::uint32_t entry_count = 8;
    static constexpr std::uint32_t payload_bytes = entry_count * 6;
    static constexpr std::size_t frame_bytes = http2_frame_header_bytes + payload_bytes;
};

static_assert(http2_local_settings::enable_push <= 1);
static_assert(http2_local_settings::enable_connect_protocol <= 1);
static_assert(http2_local_settings::initial_window_size <= 0x7fffffffU);
static_assert(http2_local_settings::max_frame_size >= http2_default_max_frame_size);
static_assert(http2_local_settings::max_frame_size <= http2_max_frame_size_limit);

inline char* http2_write_local_settings_frame(char* out, bool enable_push = false) noexcept {
    out = http2_write_frame_header(
        out, http2_local_settings::payload_bytes, http2_frame_type::settings, 0, 0);
    out = http2_write_settings_entry(
        out, http2_setting_id::header_table_size, http2_local_settings::header_table_size);
    out =
        http2_write_settings_entry(out, http2_setting_id::enable_push, enable_push ? 1u : 0u);
    out = http2_write_settings_entry(
        out, http2_setting_id::max_concurrent_streams, http2_local_settings::max_concurrent_streams);
    out = http2_write_settings_entry(
        out, http2_setting_id::initial_window_size, http2_local_settings::initial_window_size);
    out = http2_write_settings_entry(
        out, http2_setting_id::max_frame_size, http2_local_settings::max_frame_size);
    out = http2_write_settings_entry(
        out, http2_setting_id::max_header_list_size, http2_local_settings::max_header_list_size);
    out = http2_write_settings_entry(
        out, http2_setting_id::enable_connect_protocol, http2_local_settings::enable_connect_protocol);
    return http2_write_settings_entry(out, http2_setting_id::no_rfc7540_priorities, 1);
}

}  // namespace ruvia::detail
