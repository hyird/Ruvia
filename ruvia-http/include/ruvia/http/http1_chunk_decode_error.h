#pragma once

#include <cstdint>

namespace ruvia {

enum class http1_chunk_decode_error : std::uint8_t {
    invalid_framing,
    body_limit_exceeded,
    framing_limit_exceeded,
};

}  // namespace ruvia
