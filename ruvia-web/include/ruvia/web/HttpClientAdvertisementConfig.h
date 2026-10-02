#pragma once

#include <cstddef>

namespace ruvia {

// Connection advertisements are optional observations. They do not change the
// fixed registered origin, connection coalescing, TLS identity or cache policy.
struct HttpClientAdvertisementConfig final {
    bool receiveOrigins{false};
    bool receiveAlternativeServices{false};
    std::size_t maxQueuedAdvertisements{32};
    // Includes observations retained by callers after polling the client.
    std::size_t maxRetainedBytes{64 * 1024};
};

}  // namespace ruvia
