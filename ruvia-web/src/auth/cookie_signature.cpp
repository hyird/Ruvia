#include "auth/cookie_signature.h"

#include <array>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <span>
#include <stdexcept>
#include <string>

#include <openssl/evp.h>

#include "ruvia/core/base64.h"
#include "ruvia/core/constant_time.h"

namespace ruvia::detail {

namespace {

inline constexpr std::size_t hmac_sha256_size = 32;
inline constexpr std::size_t max_hmac_parameter_bytes =
    static_cast<std::size_t>((std::numeric_limits<int>::max)());
static_assert(cookie_signature_size == ruvia::base64_encoded_size(hmac_sha256_size));

}  // namespace

void write_cookie_signature(
    char* output, std::string_view secret, std::string_view name, std::string_view value) {
    if (secret.empty()) {
        throw std::invalid_argument("signed cookie secret must not be empty");
    }
    constexpr auto max_cookie_name_bytes =
        static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)());
    if (secret.size() > max_hmac_parameter_bytes) {
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
    // Assemble lenPrefix||name||value for the one-shot HMAC. A stack arena keeps
    // signing a typical cookie allocation-free; an oversized name/value spills to
    // the default upstream resource transparently.
    if (name.size() > std::numeric_limits<std::size_t>::max() - name_len_bytes.size() ||
        value.size() >
            std::numeric_limits<std::size_t>::max() - name_len_bytes.size() - name.size()) {
        throw std::length_error("signed cookie message is too large");
    }
    std::array<std::byte, 512> message_buffer;
    std::pmr::monotonic_buffer_resource message_arena(message_buffer.data(), message_buffer.size());
    std::pmr::string message(&message_arena);
    const auto message_size = name_len_bytes.size() + name.size() + value.size();
    if (message_size > max_hmac_parameter_bytes) {
        throw std::length_error("signed cookie message is too large for HMAC");
    }
    message.reserve(message_size);
    message.append(name_len_bytes.data(), name_len_bytes.size());
    message.append(name.data(), name.size());
    message.append(value.data(), value.size());

    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    std::size_t digest_size = 0;
    if (EVP_Q_mac(nullptr, "HMAC", nullptr, "SHA256", nullptr, secret.data(), secret.size(),
            reinterpret_cast<const unsigned char*>(message.data()), message.size(), digest.data(),
            digest.size(), &digest_size) == nullptr ||
        digest_size != hmac_sha256_size) {
        throw std::runtime_error("signed cookie HMAC failed");
    }
    ruvia::encode_base64(output, std::span<const std::uint8_t>(digest.data(), digest_size));
}

bool cookie_signature_equals(std::string_view left, std::string_view right) noexcept {
    return ruvia::constant_time_bytes_equal(left, right);
}

}  // namespace ruvia::detail
