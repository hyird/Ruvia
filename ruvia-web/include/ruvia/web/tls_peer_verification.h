#pragma once

#include <cstdint>

namespace ruvia {

enum class tls_peer_verification_policy : std::uint8_t {
    verify,
    skip_verification,
};

}  // namespace ruvia
