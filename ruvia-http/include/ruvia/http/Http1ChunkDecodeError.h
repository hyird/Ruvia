#pragma once

#include <cstdint>

namespace ruvia {

enum class Http1ChunkDecodeError : std::uint8_t {
    kInvalidFraming,
    kBodyLimitExceeded,
    kFramingLimitExceeded,
};

}  // namespace ruvia
