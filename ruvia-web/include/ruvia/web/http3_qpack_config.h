#pragma once

#include <cstddef>

namespace ruvia {

// Per-connection receive limits advertised in HTTP/3 SETTINGS. A zero table
// capacity disables dynamic entries; zero blocked streams forbids references
// that require encoder instructions not yet received.
struct http3_qpack_config final {
    std::size_t max_table_capacity_{4096};
    std::size_t max_blocked_streams_{16};
};

}  // namespace ruvia
