#include "crypto.h"

#include <cstring>
#include <stdexcept>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

namespace ruvia {
namespace {

[[nodiscard]] constexpr const char* openssl_digest_name(hmac_digest digest) noexcept {
    switch (digest) {
        case hmac_digest::sha256:
            return "SHA256";
        case hmac_digest::sha384:
            return "SHA384";
        case hmac_digest::sha512:
            return "SHA512";
    }
    return nullptr;
}

}  // namespace

bool constant_time_bytes_equal(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    return left.empty() || CRYPTO_memcmp(left.data(), right.data(), left.size()) == 0;
}

bool compute_sha256(std::span<const unsigned char> bytes, std::span<unsigned char, sha256_digest_size> digest) noexcept {
    unsigned int size = 0;
    return EVP_Digest(bytes.data(), bytes.size(), digest.data(), &size, EVP_sha256(), nullptr) == 1 && size == digest.size();
}

sha256::~sha256() {
    EVP_MD_CTX_free(context_);
}

void sha256::update(std::string_view bytes) {
    if (bytes.empty()) {
        return;
    }
    if (context_ == nullptr) {
        if (bytes.size() <= buffer_.size() - buffered_) {
            std::memcpy(buffer_.data() + buffered_, bytes.data(), bytes.size());
            buffered_ += bytes.size();
            return;
        }
        context_ = EVP_MD_CTX_new();
        if (context_ == nullptr || EVP_DigestInit_ex(context_, EVP_sha256(), nullptr) != 1 || (buffered_ != 0 && EVP_DigestUpdate(context_, buffer_.data(), buffered_) != 1)) {
            throw_digest_failure();
        }
    }
    if (EVP_DigestUpdate(context_, bytes.data(), bytes.size()) != 1) {
        throw_digest_failure();
    }
}

void sha256::update_u64_le(std::uint64_t value) {
    std::array<char, 8> encoded;
    for (unsigned index = 0; index < encoded.size(); ++index) {
        encoded[index] = static_cast<char>((value >> (index * 8)) & 255);
    }
    update({encoded.data(), encoded.size()});
}

sha256_digest sha256::finish() {
    sha256_digest digest;
    if (context_ == nullptr) {
        if (!compute_sha256(std::span<const unsigned char>(buffer_.data(), buffered_), digest)) {
            throw_digest_failure();
        }
        return digest;
    }
    unsigned int size = 0;
    if (EVP_DigestFinal_ex(context_, digest.data(), &size) != 1 || size != digest.size()) {
        throw_digest_failure();
    }
    return digest;
}

[[noreturn]] void sha256::throw_digest_failure() {
    throw std::runtime_error("SHA-256 digest failed");
}

sha256_digest sha256_of(std::string_view bytes) {
    sha256 digest;
    digest.update(bytes);
    return digest.finish();
}

bool compute_hmac(hmac_digest digest, std::string_view key, std::string_view message, std::span<std::uint8_t> output) noexcept {
    const auto* const digest_name = openssl_digest_name(digest);
    const auto digest_size = hmac_digest_size(digest);
    if (digest_name == nullptr || key.size() > hmac_max_input_size || message.size() > hmac_max_input_size || output.size() < digest_size) {
        return false;
    }
    std::size_t mac_size = 0;
    return EVP_Q_mac(nullptr, "HMAC", nullptr, digest_name, nullptr, key.data(), key.size(), reinterpret_cast<const unsigned char*>(message.data()), message.size(), reinterpret_cast<unsigned char*>(output.data()), output.size(), &mac_size) != nullptr && mac_size == digest_size;
}

bool fill_random_bytes(std::span<unsigned char> bytes) noexcept {
    return RAND_bytes_ex(nullptr, bytes.data(), bytes.size(), 0) == 1;
}

}  // namespace ruvia
