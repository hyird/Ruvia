#include <array>
#include <cstddef>
#include <limits>
#include <stdexcept>

#include <openssl/evp.h>

#include "ruvia/core/ConstantTime.h"

#include "auth/JwtPrimitives.h"

namespace ruvia::detail {
namespace {

struct HmacDigest final {
    const char* name;
    std::size_t size;
};

[[nodiscard]] HmacDigest digestFor(JwtAlgorithm algorithm) {
    switch (algorithm) {
        case JwtAlgorithm::kHs256:
            return {"SHA256", 32};
        case JwtAlgorithm::kHs384:
            return {"SHA384", 48};
        case JwtAlgorithm::kHs512:
            return {"SHA512", 64};
    }
    throw std::invalid_argument("unsupported JWT algorithm");
}

void validateSecret(std::string_view secret, std::size_t minimumBytes) {
    if (secret.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        throw std::length_error("JWT secret is too large");
    }
    // RFC 7518 section 3.2: the key must be at least as large as the hash output.
    if (secret.size() < minimumBytes) {
        throw std::invalid_argument("JWT secret is shorter than the selected algorithm's digest");
    }
}

void validateHmacData(std::string_view data) {
    if (data.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        throw std::length_error("JWT signing input is too large");
    }
}

}  // namespace

std::string_view jwtAlgorithmName(JwtAlgorithm algorithm) {
    switch (algorithm) {
        case JwtAlgorithm::kHs256:
            return "HS256";
        case JwtAlgorithm::kHs384:
            return "HS384";
        case JwtAlgorithm::kHs512:
            return "HS512";
    }
    return {};
}

std::pmr::string jwtHmacSign(JwtAlgorithm algorithm, std::string_view secret, std::string_view data,
    std::pmr::memory_resource* resource) {
    const auto method = digestFor(algorithm);
    validateSecret(secret, method.size);
    validateHmacData(data);
    std::size_t length = 0;
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    if (EVP_Q_mac(nullptr, "HMAC", nullptr, method.name, nullptr, secret.data(), secret.size(),
            reinterpret_cast<const unsigned char*>(data.data()), data.size(), digest.data(),
            digest.size(), &length) == nullptr ||
        length != method.size) {
        throw std::runtime_error("JWT HMAC signing failed");
    }
    return jwtBase64UrlEncode(
        std::string_view(reinterpret_cast<const char*>(digest.data()), length), resource);
}

bool jwtConstantTimeEquals(std::string_view left, std::string_view right) noexcept {
    return ruvia::constantTimeBytesEqual(left, right);
}

}  // namespace ruvia::detail
