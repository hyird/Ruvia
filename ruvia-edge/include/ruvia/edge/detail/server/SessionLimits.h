#pragma once

#include <chrono>
#include <cstddef>

namespace ruvia::edge {

// Upper bound on a whole buffered client request (head plus any forwarded body).
inline constexpr std::size_t kMaxRequestBytes = 1u * 1024u * 1024u;

// How long a persistent client connection may sit idle awaiting its next request.
inline constexpr std::chrono::seconds kKeepAliveIdleTimeout{60};

}  // namespace ruvia::edge
