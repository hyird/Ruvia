#pragma once

#include <cstdint>

namespace ruvia {

enum class http3_connection_error_scope : std::uint8_t {
    none,
    stream,
    connection,
};

enum class http3_connection_error_code : std::uint64_t {
    no_error = 0x100,
    general_protocol_error = 0x101,
    internal_error = 0x102,
    stream_creation_error = 0x103,
    closed_critical_stream = 0x104,
    frame_unexpected = 0x105,
    frame_error = 0x106,
    id_error = 0x108,
    settings_error = 0x109,
    missing_settings = 0x10a,
    request_rejected = 0x10b,
    request_cancelled = 0x10c,
    message_error = 0x10e,
    qpack_decompression_failed = 0x200,
    qpack_encoder_stream_error = 0x201,
    qpack_decoder_stream_error = 0x202,
    excessive_load = 0x107,
};

}  // namespace ruvia
