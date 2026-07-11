#pragma once

#include <cstdint>

namespace ruvia {

// Protocol version is HTTP message control data, not a borrowed wire token.
// HTTP/1 carries it in the start-line; HTTP/2 establishes it from the
// connection framing even though no textual version appears in a field block.
enum class HttpProtocolVersion : std::uint8_t {
    kHttp10,
    kHttp11,
    kHttp2,
};

}  // namespace ruvia
