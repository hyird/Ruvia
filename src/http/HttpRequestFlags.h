#pragma once

#include <cstdint>

namespace ruvia::detail {

struct HttpRequestFlags {
    bool connectionClose{false};
    bool connectionKeepAlive{false};
    bool expectContinue{false};
    bool upgrade{false};
    bool hasHost{false};
    std::uint8_t secWebSocketKeyCount{0};
    std::uint8_t secWebSocketVersionCount{0};
    std::uint8_t secWebSocketProtocolCount{0};
};

}  // namespace ruvia::detail
