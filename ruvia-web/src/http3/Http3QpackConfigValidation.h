#pragma once

#include <stdexcept>

#include "ruvia/http/Http3VarInt.h"
#include "ruvia/web/Http3QpackConfig.h"

namespace ruvia::detail {
inline void validateHttp3QpackConfig(Http3QpackConfig config) {
    if (config.maxTableCapacity > kHttp3VarIntMax || config.maxBlockedStreams > kHttp3VarIntMax) {
        throw std::invalid_argument("HTTP/3 QPACK limits exceed the QUIC variable integer range");
    }
}
}  // namespace ruvia::detail
