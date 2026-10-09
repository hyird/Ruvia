#include <array>
#include <cstddef>
#include <limits>
#include <stdexcept>

#include <openssl/evp.h>

#include "ruvia/core/constant_time.h"

#include "auth/jwt_primitives.h"

namespace ruvia::detail {
namespace {

struct hmac_digest final {
    const char* name_;
    std::size_t size_;
};

[[nodiscard]] hmac_digest digest_for(jwt_algorithm algorithm) {
    switch (algorithm) {
        case jwt_algorithm::hs256:
            return {"SHA256", 32};
        case jwt_algorithm::hs384:
            return {"SHA384", 48};
        case jwt_algorithm::hs512:
            return {"SHA512", 64};
    }
    throw std::invalid_argument("unsupported JWT algorithm");
}

void validate_secret(std::string_view secret, std::size_t minimum_bytes) {
    if (secret.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        throw std::length_error("JWT secret is too large");
    }
    // RFC 7518 section 3.2: the key must be at least as large as the hash output.
    if (secret.size() < minimum_bytes) {
        throw std::invalid_argument("JWT secret is shorter than the selected algorithm's digest");
    }
}

void validate_hmac_data(std::string_view data) {
    if (data.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        throw std::length_error("JWT signing input is too large");
    }
}

}  // namespace

std::string_view jwt_algorithm_name(jwt_algorithm algorithm) {
    switch (algorithm) {
        case jwt_algorithm::hs256:
            return "HS256";
        case jwt_algorithm::hs384:
            return "HS384";
        case jwt_algorithm::hs512:
            return "HS512";
    }
    return {};
}

std::pmr::string jwt_hmac_sign(jwt_algorithm algorithm, std::string_view secret, std::string_view data,
    std::pmr::memory_resource* resource) {
    const auto method = digest_for(algorithm);
    validate_secret(secret, method.size_);
    validate_hmac_data(data);
    std::size_t length = 0;
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    if (EVP_Q_mac(nullptr, "HMAC", nullptr, method.name_, nullptr, secret.data(), secret.size(),
            reinterpret_cast<const unsigned char*>(data.data()), data.size(), digest.data(),
            digest.size(), &length) == nullptr ||
        length != method.size_) {
        throw std::runtime_error("JWT HMAC signing failed");
    }
    return jwt_base64_url_encode(
        std::string_view(reinterpret_cast<const char*>(digest.data()), length), resource);
}

bool jwt_constant_time_equals(std::string_view left, std::string_view right) noexcept {
    return ruvia::constant_time_bytes_equal(left, right);
}

}  // namespace ruvia::detail
