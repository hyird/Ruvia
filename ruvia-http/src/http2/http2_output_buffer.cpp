#include "http2/http2_output_buffer.h"

#include <array>

namespace ruvia::detail {

void http2_output_buffer::append_goaway_frame(
    std::uint32_t last_stream_id, http2_error_code error, std::string_view debug) {
    std::array<char, 8> payload;
    auto* const end = http2_write_goaway_payload(payload.data(), last_stream_id, error);
    append_frame(http2_frame_type::goaway, 0, 0,
        std::string_view(payload.data(), static_cast<std::size_t>(end - payload.data())), debug);
}

void http2_output_buffer::append_rst_stream(std::uint32_t stream_id, http2_error_code error) {
    std::array<char, 4> payload;
    auto* const end = http2_write32(payload.data(), static_cast<std::uint32_t>(error));
    append_frame(http2_frame_type::rst_stream, 0, stream_id,
        std::string_view(payload.data(), static_cast<std::size_t>(end - payload.data())));
}

}  // namespace ruvia::detail
