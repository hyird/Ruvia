#pragma once

#include <cstdint>

#include "ruvia/http/hpack.h"

namespace ruvia::detail {

enum class header_decode_status : std::uint8_t { ok,
    protocol_error,
    compression_error };

[[nodiscard]] inline header_decode_status http2_classify_header_decode_result(
    std::optional<hpack_decode_error> error) noexcept {
    if (!error.has_value()) {
        return header_decode_status::ok;
    }
    return *error == hpack_decode_error::callback_rejected
               ? header_decode_status::protocol_error
               : header_decode_status::compression_error;
}

}  // namespace ruvia::detail
