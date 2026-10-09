#include "ruvia/http/http3_local_critical_streams.h"

#include <variant>

#include "ruvia/http/http3_frames.h"
#include "ruvia/http/http3_var_int.h"

namespace ruvia {

std::variant<http3_local_critical_streams, http3_local_critical_streams_error>
http3_local_critical_streams::create(const http3_settings& settings) noexcept {
    http3_local_critical_streams streams;
    const auto stream_type_value = encode_http3_var_int(streams.control_, 0);
    if ((stream_type_value.index() != 0)) {
        return http3_local_critical_streams_error::settings_encoding_error;
    }
    std::array<char, 5 * 2 * http3_var_int_max_bytes> settings_payload{};
    const auto settings_size = encode_http3_settings(settings_payload, settings);
    if ((settings_size.index() != 0)) {
        return http3_local_critical_streams_error::settings_encoding_error;
    }
    const auto frame_header = encode_http3_frame_header(
        std::span<char>(streams.control_).subspan(std::get<0>(stream_type_value)),
        static_cast<std::uint64_t>(http3_frame_type::settings), std::get<0>(settings_size));
    if ((frame_header.index() != 0)) {
        return http3_local_critical_streams_error::settings_encoding_error;
    }
    const auto payload_offset = std::get<0>(stream_type_value) + std::get<0>(frame_header);
    if (payload_offset > streams.control_.size() || std::get<0>(settings_size) > streams.control_.size() - payload_offset) {
        return http3_local_critical_streams_error::settings_encoding_error;
    }
    for (std::size_t i = 0; i < std::get<0>(settings_size); ++i) {
        streams.control_[payload_offset + i] = settings_payload[i];
    }
    streams.control_size_ = payload_offset + std::get<0>(settings_size);

    const auto encoder_type = encode_http3_var_int(streams.qpack_encoder_, 2);
    const auto decoder_type = encode_http3_var_int(streams.qpack_decoder_, 3);
    if ((encoder_type.index() != 0) || (decoder_type.index() != 0)) {
        return http3_local_critical_streams_error::settings_encoding_error;
    }
    streams.qpack_encoder_size_ = std::get<0>(encoder_type);
    streams.qpack_decoder_size_ = std::get<0>(decoder_type);
    return streams;
}

}  // namespace ruvia
