#include "ruvia/web/detail/auth/JwtInternal.h"

#include "ruvia/detail/ConstantTime.h"

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <array>
#include <stdexcept>

namespace ruvia::detail {
namespace {

[[nodiscard]] const EVP_MD* digestFor(JwtAlgorithm algorithm) {
    switch (algorithm) {
        case JwtAlgorithm::kHs256: return EVP_sha256();
        case JwtAlgorithm::kHs384: return EVP_sha384();
        case JwtAlgorithm::kHs512: return EVP_sha512();
    }
    throw std::invalid_argument("unsupported JWT algorithm");
}

void validateSecret(std::string_view secret) {
    if (secret.empty()) {
        throw std::invalid_argument("JWT secret must not be empty");
    }
}

}  // namespace

std::string_view jwtAlgorithmName(JwtAlgorithm algorithm) {
    switch (algorithm) {
        case JwtAlgorithm::kHs256: return "HS256";
        case JwtAlgorithm::kHs384: return "HS384";
        case JwtAlgorithm::kHs512: return "HS512";
    }
    return {};
}

std::pmr::string jwtHmacSign(
    JwtAlgorithm algorithm,
    std::string_view secret,
    std::string_view data,
    std::pmr::memory_resource* resource) {
    validateSecret(secret);
    unsigned int length = 0;
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    if (HMAC(
            digestFor(algorithm),
            secret.data(),
            static_cast<int>(secret.size()),
            reinterpret_cast<const unsigned char*>(data.data()),
            data.size(),
            digest.data(),
            &length) == nullptr) {
        throw std::runtime_error("JWT HMAC signing failed");
    }
    return jwtBase64UrlEncode(
        std::string_view(reinterpret_cast<const char*>(digest.data()), length),
        resource);
}

bool jwtConstantTimeEquals(std::string_view left, std::string_view right) noexcept {
    return constantTimeBytesEqual(left, right);
}

}  // namespace ruvia::detail
