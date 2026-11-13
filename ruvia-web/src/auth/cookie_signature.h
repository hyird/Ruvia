#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string_view>

#include <openssl/evp.h>

#include "ruvia/http/http_base64.h"

#include "http/inline_http_header_views.h"

namespace ruvia::detail {
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
[[nodiscard]] inline bool compute_hmac(
    hmac_digest digest, std::string_view key, std::string_view message, std::span<std::uint8_t> output) noexcept {
    const auto* const digest_name = openssl_digest_name(digest);
    const auto digest_size = hmac_digest_size(digest);
    if (digest_name == nullptr || key.size() > hmac_max_input_size || message.size() > hmac_max_input_size ||
        output.size() < digest_size) {
        return false;
    }
    std::size_t mac_size = 0;
    return EVP_Q_mac(nullptr, "HMAC", nullptr, digest_name, nullptr, key.data(), key.size(),
               reinterpret_cast<const unsigned char*>(message.data()), message.size(),
               reinterpret_cast<unsigned char*>(output.data()), output.size(), &mac_size) != nullptr &&
           mac_size == digest_size;
}
}  // namespace ruvia::detail

namespace ruvia::detail {

// base64(HMAC-SHA256) — fixed width, standard alphabet with padding.
inline constexpr std::size_t cookie_signature_size = 44;
static_assert(cookie_signature_size == http_base64_encoded_size(hmac_digest_size(hmac_digest::sha256)));

// Writes exactly cookie_signature_size characters to `output`. The cookie `name`
// is bound into the MAC (length-framed together with `value`) so a value signed
// for one cookie is not a valid signature for another under the same secret.
// Throws std::invalid_argument on an empty secret and std::length_error when
// the secret or length-framed message cannot be represented by OpenSSL's HMAC
// length parameters.
inline void write_cookie_signature(
    char* output, std::string_view secret, std::string_view name, std::string_view value) {
    if (secret.empty()) {
        throw std::invalid_argument("signed cookie secret must not be empty");
    }
    constexpr auto max_cookie_name_bytes =
        static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)());
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
    if (name.size() > hmac_max_input_size - name_len_bytes.size() ||
        value.size() > hmac_max_input_size - name_len_bytes.size() - name.size()) {
        throw std::length_error("signed cookie message is too large for HMAC");
    }
    const inline_text_concat<512> message(
        {std::string_view(name_len_bytes.data(), name_len_bytes.size()), name, value});

    std::array<std::uint8_t, hmac_digest_size(hmac_digest::sha256)> digest;
    if (!compute_hmac(hmac_digest::sha256, secret, message.view(), digest)) {
        throw std::runtime_error("signed cookie HMAC computation failed");
    }
    http_base64_encode(output, std::span<const std::uint8_t>(digest));
}
}  // namespace ruvia::detail
