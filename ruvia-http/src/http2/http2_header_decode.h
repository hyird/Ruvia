#pragma once

#include <cstdint>

#include "ruvia/http/hpack_protocol_types.h"

namespace ruvia::detail {

enum class header_decode_status : std::uint8_t { ok,
    protocol_error,
    compression_error };

[[nodiscard]] inline header_decode_status http2_classify_header_decode_result(
    const hpack_decode_result& result_value) noexcept {
    if (result_value.decoded()) {
        return header_decode_status::ok;
    }
    return result_value.error() == hpack_decode_error::callback_rejected
               ? header_decode_status::protocol_error
               : header_decode_status::compression_error;
}

}  // namespace ruvia::detail
