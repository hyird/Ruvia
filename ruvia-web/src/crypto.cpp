#include "crypto.h"

#include <cstring>
#include <stdexcept>
#include <utility>

#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include "binary_text_encoding.h"
#include "inline_storage.h"

namespace ruvia {
namespace {

static_assert(cookie_signature_size == base64_encoded_size(hmac_digest_size(hmac_digest::sha256)));

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

bool constant_time_bytes_equal(byte_view left, byte_view right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    return left.empty() || CRYPTO_memcmp(left.data(), right.data(), left.size()) == 0;
}

bool compute_sha256(byte_view bytes, std::span<std::byte, sha256_digest_size> digest) noexcept {
    unsigned int size = 0;
    return EVP_Digest(bytes.data(), bytes.size(), reinterpret_cast<unsigned char*>(digest.data()), &size, EVP_sha256(), nullptr) == 1 && size == digest.size();
}

sha256::~sha256() {
    EVP_MD_CTX_free(context_);
}

void sha256::update(byte_view bytes) {
    if (state_ != state_type::active) {
        throw std::logic_error("SHA-256 digest is no longer active");
    }
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
            state_ = state_type::failed;
            throw_digest_failure();
        }
    }
    if (EVP_DigestUpdate(context_, bytes.data(), bytes.size()) != 1) {
        state_ = state_type::failed;
        throw_digest_failure();
    }
}

void sha256::update_u64_le(std::uint64_t value) {
    std::array<std::byte, 8> encoded{};
    for (unsigned index = 0; index < encoded.size(); ++index) {
        encoded[index] = static_cast<std::byte>((value >> (index * 8)) & 255U);
    }
    update(byte_view(encoded));
}

sha256_digest sha256::finish() {
    if (state_ != state_type::active) {
        throw std::logic_error("SHA-256 digest is no longer active");
    }
    sha256_digest digest{};
    if (context_ == nullptr) {
        if (!compute_sha256(byte_view(buffer_.data(), buffered_), digest)) {
            state_ = state_type::failed;
            throw_digest_failure();
        }
        state_ = state_type::finished;
        return digest;
    }
    unsigned int size = 0;
    if (EVP_DigestFinal_ex(context_, reinterpret_cast<unsigned char*>(digest.data()), &size) != 1 || size != digest.size()) {
        state_ = state_type::failed;
        throw_digest_failure();
    }
    state_ = state_type::finished;
    return digest;
}

void sha256::reset() noexcept {
    buffered_ = 0;
    state_ = state_type::active;
    if (context_ != nullptr) {
        if (EVP_MD_CTX_reset(context_) != 1 || EVP_DigestInit_ex(context_, EVP_sha256(), nullptr) != 1) {
            ERR_clear_error();
            EVP_MD_CTX_free(std::exchange(context_, nullptr));
        }
    }
}

[[noreturn]] void sha256::throw_digest_failure() {
    throw std::runtime_error("SHA-256 digest failed");
}

sha256_digest sha256_of(byte_view bytes) {
    sha256 digest;
    digest.update(bytes);
    return digest.finish();
}

bool compute_hmac(hmac_digest digest, byte_view key, byte_view message, mutable_byte_view output) noexcept {
    const auto* const digest_name = openssl_digest_name(digest);
    const auto digest_size = hmac_digest_size(digest);
    if (digest_name == nullptr || key.size() > hmac_max_input_size || message.size() > hmac_max_input_size || output.size() < digest_size) {
        return false;
    }
    std::size_t mac_size = 0;
    return EVP_Q_mac(nullptr, "HMAC", nullptr, digest_name, nullptr, key.data(), key.size(), reinterpret_cast<const unsigned char*>(message.data()), message.size(), reinterpret_cast<unsigned char*>(output.data()), output.size(), &mac_size) != nullptr && mac_size == digest_size;
}

bool fill_random_bytes(mutable_byte_view bytes) noexcept {
    return RAND_bytes_ex(nullptr, reinterpret_cast<unsigned char*>(bytes.data()), bytes.size(), 0) == 1;
}

void write_cookie_signature(std::span<char, cookie_signature_size> output, std::string_view secret, std::string_view name, std::string_view value) {
    if (secret.empty()) {
        throw std::invalid_argument("signed cookie secret must not be empty");
    }
    constexpr auto max_cookie_name_bytes = static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)());
    if (secret.size() > hmac_max_input_size) {
        throw std::length_error("signed cookie secret is too large");
    }
    if (name.size() > max_cookie_name_bytes) {
        throw std::length_error("signed cookie name is too large");
    }
    const auto name_len = static_cast<std::uint32_t>(name.size());
    const std::array<char, 4> name_len_bytes{
        static_cast<char>((name_len >> 24) & 0xFF),
        static_cast<char>((name_len >> 16) & 0xFF),
        static_cast<char>((name_len >> 8) & 0xFF),
        static_cast<char>(name_len & 0xFF),
    };
    if (name.size() > hmac_max_input_size - name_len_bytes.size() || value.size() > hmac_max_input_size - name_len_bytes.size() - name.size()) {
        throw std::length_error("signed cookie message is too large for HMAC");
    }
    const detail::inline_text_concat<512> message({std::string_view(name_len_bytes.data(), name_len_bytes.size()), name, value});

    std::array<std::byte, hmac_digest_size(hmac_digest::sha256)> mac{};
    if (!compute_hmac(hmac_digest::sha256, secret, message.view(), mac)) {
        throw std::runtime_error("signed cookie HMAC computation failed");
    }
    (void)base64_encode(output, mac);
}

std::string sign_cookie_value(std::string_view secret, std::string_view name, std::string_view value) {
    std::array<char, cookie_signature_size> signature{};
    write_cookie_signature(signature, secret, name, value);
    std::string out;
    out.reserve(value.size() + 1 + signature.size());
    out.append(value);
    out.push_back('.');
    out.append(signature.data(), signature.size());
    return out;
}

std::optional<std::string_view> verify_signed_cookie_value(std::string_view secret, std::string_view name, std::string_view signed_value) {
    if (secret.empty()) {
        throw std::invalid_argument("signed cookie secret must not be empty");
    }
    if (signed_value.size() <= cookie_signature_size) {
        return std::nullopt;
    }
    const auto value_size = signed_value.size() - cookie_signature_size - 1;
    if (signed_value[value_size] != '.') {
        return std::nullopt;
    }
    const auto value = signed_value.substr(0, value_size);
    const auto provided_signature = signed_value.substr(value_size + 1);
    std::array<char, cookie_signature_size> expected{};
    write_cookie_signature(expected, secret, name, value);
    if (!constant_time_bytes_equal(provided_signature, std::string_view(expected.data(), expected.size()))) {
        return std::nullopt;
    }
    return value;
}

}  // namespace ruvia
