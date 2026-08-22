#pragma once

#include <cstdint>

namespace ruvia {

enum class TcpNoDelayPolicy : std::uint8_t {
    kSystemDefault,
    kEnable,
};

enum class TcpKeepAlivePolicy : std::uint8_t {
    kSystemDefault,
    kEnable,
};

}  // namespace ruvia
