#include "CookieSignature.h"

#include "../core/Base64.h"

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <array>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <stdexcept>
#include <string>

namespace ruvia::detail {

namespace {

inline constexpr std::size_t kHmacSha256Size = 32;
static_assert(kCookieSignatureSize == base64EncodedSize(kHmacSha256Size));

}  // namespace

void writeCookieSignature(
    char* output, std::string_view secret, std::string_view name, std::string_view value) {
    if (secret.empty()) {
        throw std::invalid_argument("signed cookie secret must not be empty");
    }
    // Length-frame the name so the name/value boundary is unambiguous regardless
    // of the bytes either contains (prevents name||value collisions).
    const auto nameLen = static_cast<std::uint32_t>(name.size());
    const std::array<char, 4> nameLenBytes{
        static_cast<char>((nameLen >> 24) & 0xFF),
        static_cast<char>((nameLen >> 16) & 0xFF),
        static_cast<char>((nameLen >> 8) & 0xFF),
        static_cast<char>(nameLen & 0xFF),
    };
    // Assemble lenPrefix||name||value for the one-shot HMAC. A stack arena keeps
    // signing a typical cookie allocation-free; an oversized name/value spills to
    // the default upstream resource transparently.
    std::array<std::byte, 512> messageBuffer;
    std::pmr::monotonic_buffer_resource messageArena(messageBuffer.data(), messageBuffer.size());
    std::pmr::string message(&messageArena);
    message.reserve(nameLenBytes.size() + name.size() + value.size());
    message.append(nameLenBytes.data(), nameLenBytes.size());
    message.append(name.data(), name.size());
    message.append(value.data(), value.size());

    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digestSize = 0;
    if (HMAC(
            EVP_sha256(),
            secret.data(),
            static_cast<int>(secret.size()),
            reinterpret_cast<const unsigned char*>(message.data()),
            message.size(),
            digest.data(),
            &digestSize) == nullptr ||
        digestSize != kHmacSha256Size) {
        throw std::runtime_error("signed cookie HMAC failed");
    }
    encodeBase64(output, std::span<const std::uint8_t>(digest.data(), digestSize));
}

bool cookieSignatureEquals(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    unsigned char diff = 0;
    for (std::size_t i = 0; i < left.size(); ++i) {
        diff |= static_cast<unsigned char>(left[i] ^ right[i]);
    }
    return diff == 0;
}

}  // namespace ruvia::detail
