#include "tls/tls_random.h"

#include <openssl/rand.h>

namespace ruvia::detail {

bool fill_random_bytes(std::span<unsigned char> bytes) noexcept {
    return RAND_bytes_ex(nullptr, bytes.data(), bytes.size(), 0) == 1;
}

}  // namespace ruvia::detail
