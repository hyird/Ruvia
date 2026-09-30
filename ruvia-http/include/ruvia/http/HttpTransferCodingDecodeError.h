#pragma once

#include <cstdint>

namespace ruvia {

enum class HttpTransferCodingDecodeError : std::uint8_t {
    kInvalidContent,
    kDecodedSizeExceeded,
};

}  // namespace ruvia
