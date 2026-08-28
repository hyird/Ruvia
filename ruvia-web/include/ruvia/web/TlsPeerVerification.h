#pragma once

#include <cstdint>

namespace ruvia {

enum class TlsPeerVerificationPolicy : std::uint8_t {
    kVerify,
    kSkipVerification,
};

}  // namespace ruvia
