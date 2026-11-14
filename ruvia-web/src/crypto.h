#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>
#include <string_view>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include "borrowed_text.h"
#include "inline_storage.h"

// OpenSSL-backed secrets handling shared by sessions, signed cookies, CSRF,
// JWT, QUIC, TLS and the database layer: constant-time comparison, SHA-256,
// one-shot HMAC, and the CSPRNG.
namespace ruvia::detail {

// Length-checked, data-independent (constant-time) byte comparison for secrets
// and authentication tags. It never
// early-outs on the first differing byte, so the time taken does not leak how
// far an attacker-supplied value matched the expected one. Unequal lengths are
// reported immediately; that is not secret since these values are fixed-length.
[[nodiscard]] inline bool constant_time_bytes_equal(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    return left.empty() || CRYPTO_memcmp(left.data(), right.data(), left.size()) == 0;
}

inline constexpr std::size_t sha256_digest_size = 32;
using sha256_digest = std::array<unsigned char, sha256_digest_size>;

// One-shot SHA-256 into `digest`. Returns false only when OpenSSL fails, so
// noexcept callers such as handshake callbacks can map it to their own failure.
[[nodiscard]] inline bool compute_sha256(std::span<const unsigned char> bytes, std::span<unsigned char, sha256_digest_size> digest) noexcept {
    unsigned int size = 0;
    return EVP_Digest(bytes.data(), bytes.size(), digest.data(), &size, EVP_sha256(), nullptr) == 1 && size == digest.size();
}

// SHA-256 over input fed in pieces. Input that fits the inline buffer is
// digested in one call; longer input streams through an OpenSSL context
// created once it overflows. Failures throw std::runtime_error.
class sha256 final {
public:
    sha256() noexcept = default;

    ~sha256() {
        EVP_MD_CTX_free(context_);
    }

    sha256(const sha256&) = delete;
    sha256& operator=(const sha256&) = delete;

    void update(std::string_view bytes) {
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

    // Eight little-endian bytes.
    void update_u64_le(std::uint64_t value) {
        std::array<char, 8> encoded;
        for (unsigned index = 0; index < encoded.size(); ++index) {
            encoded[index] = static_cast<char>((value >> (index * 8)) & 255);
        }
        update({encoded.data(), encoded.size()});
    }

    // Ends the digest; nothing may be fed afterwards.
    [[nodiscard]] sha256_digest finish() {
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

private:
    [[noreturn]] static void throw_digest_failure() {
        throw std::runtime_error("SHA-256 digest failed");
    }

    std::array<unsigned char, 512> buffer_;
    std::size_t buffered_{0};
    EVP_MD_CTX* context_{nullptr};
};

[[nodiscard]] inline sha256_digest sha256_of(std::string_view bytes) {
    sha256 digest;
    digest.update(bytes);
    return digest.finish();
}

// The digests behind signed cookies (SHA-256) and JWT HS256/HS384/HS512.
enum class hmac_digest : std::uint8_t {
    sha256,
    sha384,
    sha512,
};

inline constexpr std::size_t hmac_max_digest_size = 64;
// OpenSSL's one-shot MAC takes int-sized key and message lengths.
inline constexpr std::size_t hmac_max_input_size = static_cast<std::size_t>((std::numeric_limits<int>::max)());

[[nodiscard]] constexpr std::size_t hmac_digest_size(hmac_digest digest) noexcept {
    switch (digest) {
        case hmac_digest::sha256:
            return 32;
        case hmac_digest::sha384:
            return 48;
        case hmac_digest::sha512:
            return 64;
    }
    return 0;
}

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

// Writes the hmac_digest_size(digest) bytes of HMAC(key, message) to the front
// of `output`. Returns false when key or message exceeds hmac_max_input_size,
// `output` is too small, or OpenSSL fails; callers check the sizes first to
// report them in their own terms.
[[nodiscard]] inline bool compute_hmac(hmac_digest digest, std::string_view key, std::string_view message, std::span<std::uint8_t> output) noexcept {
    const auto* const digest_name = openssl_digest_name(digest);
    const auto digest_size = hmac_digest_size(digest);
    if (digest_name == nullptr || key.size() > hmac_max_input_size || message.size() > hmac_max_input_size || output.size() < digest_size) {
        return false;
    }
    std::size_t mac_size = 0;
    return EVP_Q_mac(nullptr, "HMAC", nullptr, digest_name, nullptr, key.data(), key.size(), reinterpret_cast<const unsigned char*>(message.data()), message.size(), reinterpret_cast<unsigned char*>(output.data()), output.size(), &mac_size) != nullptr && mac_size == digest_size;
}

// base64(HMAC-SHA256) — fixed width, standard alphabet with padding.
inline constexpr std::size_t cookie_signature_size = 44;
static_assert(cookie_signature_size == http_base64_encoded_size(hmac_digest_size(hmac_digest::sha256)));

// Writes exactly cookie_signature_size characters to `output`. The cookie `name`
// is bound into the MAC (length-framed together with `value`) so a value signed
// for one cookie is not a valid signature for another under the same secret.
// Throws std::invalid_argument on an empty secret and std::length_error when
// the secret or length-framed message cannot be represented by OpenSSL's HMAC
// length parameters.
inline void write_cookie_signature(char* output, std::string_view secret, std::string_view name, std::string_view value) {
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
    // Length-frame the name so the name/value boundary is unambiguous regardless
    // of the bytes either contains (prevents name||value collisions).
    const auto name_len = static_cast<std::uint32_t>(name.size());
    const std::array<char, 4> name_len_bytes{
        static_cast<char>((name_len >> 24) & 0xFF),
        static_cast<char>((name_len >> 16) & 0xFF),
        static_cast<char>((name_len >> 8) & 0xFF),
        static_cast<char>(name_len & 0xFF),
    };
    // Assemble lenPrefix||name||value for the one-shot HMAC. Inline storage keeps
    // signing a typical cookie allocation-free.
    if (name.size() > hmac_max_input_size - name_len_bytes.size() || value.size() > hmac_max_input_size - name_len_bytes.size() - name.size()) {
        throw std::length_error("signed cookie message is too large for HMAC");
    }
    const inline_text_concat<512> message({std::string_view(name_len_bytes.data(), name_len_bytes.size()), name, value});

    std::array<std::uint8_t, hmac_digest_size(hmac_digest::sha256)> digest;
    if (!compute_hmac(hmac_digest::sha256, secret, message.view(), digest)) {
        throw std::runtime_error("signed cookie HMAC computation failed");
    }
    http_base64_encode(output, std::span<const std::uint8_t>(digest));
}

// Fills bytes from OpenSSL's CSPRNG. Returns false when the generator cannot
// produce output; callers translate that into their own failure contract.
[[nodiscard]] inline bool fill_random_bytes(std::span<unsigned char> bytes) noexcept {
    return RAND_bytes_ex(nullptr, bytes.data(), bytes.size(), 0) == 1;
}

// Lowercase hex characters of a token: 24 random bytes, 192 bits.
inline constexpr std::size_t secure_token_size = 48;

[[nodiscard]] inline std::string_view generate_secure_token(std::span<char, secure_token_size> buffer) {
    std::array<unsigned char, secure_token_size / 2> raw;
    if (!fill_random_bytes(raw)) {
        throw std::runtime_error("secure random generation failed");
    }
    http_encode_lower_hex(buffer.data(), std::span<const unsigned char>(raw));
    return std::string_view(buffer.data(), buffer.size());
}

}  // namespace ruvia::detail
