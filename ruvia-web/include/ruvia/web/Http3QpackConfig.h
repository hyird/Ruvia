#pragma once

#include <cstddef>

namespace ruvia {

// Per-connection receive limits advertised in HTTP/3 SETTINGS. A zero table
// capacity disables dynamic entries; zero blocked streams forbids references
// that require encoder instructions not yet received.
struct Http3QpackConfig final {
    std::size_t maxTableCapacity{4096};
    std::size_t maxBlockedStreams{16};
};

}  // namespace ruvia
