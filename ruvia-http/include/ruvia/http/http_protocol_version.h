#pragma once

#include <cstdint>

namespace ruvia {

// Protocol version is HTTP message control data, carried in the start-line.
enum class http_protocol_version : std::uint8_t {
    http10,
    http11,
};

}  // namespace ruvia
