#include "http2/http2_output_buffer.h"

#include <cstring>

namespace ruvia::detail {

void http2_output_buffer::append_goaway_frame(
    std::uint32_t last_stream_id, http2_error_code error, std::string_view debug) {
    if (debug.size() > http2_max_frame_size - 8) {
        std::terminate();
    }
    const auto payload = append_frame_payload(http2_frame_type::goaway, 0, 0, 8 + debug.size());
    auto* const debug_out = http2_write_goaway_payload(payload.data(), last_stream_id, error);
    if (!debug.empty()) {
        std::memcpy(debug_out, debug.data(), debug.size());
    }
}

void http2_output_buffer::append_rst_stream(std::uint32_t stream_id, http2_error_code error) {
    const auto payload = append_frame_payload(http2_frame_type::rst_stream, 0, stream_id, 4);
    (void)http2_write32(payload.data(), static_cast<std::uint32_t>(error));
}

}  // namespace ruvia::detail
