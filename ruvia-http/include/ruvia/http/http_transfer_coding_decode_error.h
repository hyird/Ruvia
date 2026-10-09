#pragma once

#include <cstdint>

namespace ruvia {

enum class http_transfer_coding_decode_error : std::uint8_t {
    invalid_content,
    decoded_size_exceeded,
};

}  // namespace ruvia
