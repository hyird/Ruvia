#pragma once

#include <cstdint>

namespace ruvia::detail {

struct HttpRequestFlags {
    std::uint8_t secWebSocketKeyCount{0};
    std::uint8_t secWebSocketVersionCount{0};
    std::uint8_t secWebSocketProtocolCount{0};
    bool connectionClose : 1 {false};
    bool connectionKeepAlive : 1 {false};
    bool expectContinue : 1 {false};
    bool upgrade : 1 {false};
    bool hasHost : 1 {false};
};

}  // namespace ruvia::detail
