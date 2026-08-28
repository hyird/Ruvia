#include "ruvia/web/detail/auth/CookieSignature.h"

#include "ruvia/core/detail/util/Base64.h"
#include "ruvia/core/detail/util/ConstantTime.h"

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <array>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <span>
#include <stdexcept>
#include <string>

namespace ruvia::detail {

namespace {

inline constexpr std::size_t kHmacSha256Size = 32;
inline constexpr std::size_t kMaxHmacParameterBytes = static_cast<std::size_t>((std::numeric_limits<int>::max)());
static_assert(kCookieSignatureSize == base64EncodedSize(kHmacSha256Size));

}  // namespace

void writeCookieSignature(char* output, std::string_view secret, std::string_view name, std::string_view value) {
    if (secret.empty()) {
        throw std::invalid_argument("signed cookie secret must not be empty");
    }
    constexpr auto kMaxCookieNameBytes = static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)());
    if (secret.size() > kMaxHmacParameterBytes) {
        throw std::length_error("signed cookie secret is too large");
    }
    if (name.size() > kMaxCookieNameBytes) {
        throw std::length_error("signed cookie name is too large");
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
    if (name.size() > std::numeric_limits<std::size_t>::max() - nameLenBytes.size() || value.size() > std::numeric_limits<std::size_t>::max() - nameLenBytes.size() - name.size()) {
        throw std::length_error("signed cookie message is too large");
    }
    std::array<std::byte, 512> messageBuffer;
    std::pmr::monotonic_buffer_resource messageArena(messageBuffer.data(), messageBuffer.size());
    std::pmr::string message(&messageArena);
    const auto messageSize = nameLenBytes.size() + name.size() + value.size();
    if (messageSize > kMaxHmacParameterBytes) {
        throw std::length_error("signed cookie message is too large for HMAC");
    }
    message.reserve(messageSize);
    message.append(nameLenBytes.data(), nameLenBytes.size());
    message.append(name.data(), name.size());
    message.append(value.data(), value.size());

    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digestSize = 0;
    if (HMAC(EVP_sha256(), secret.data(), static_cast<int>(secret.size()), reinterpret_cast<const unsigned char*>(message.data()), message.size(), digest.data(), &digestSize) == nullptr || digestSize != kHmacSha256Size) {
        throw std::runtime_error("signed cookie HMAC failed");
    }
    encodeBase64(output, std::span<const std::uint8_t>(digest.data(), digestSize));
}

bool cookieSignatureEquals(std::string_view left, std::string_view right) noexcept {
    return constantTimeBytesEqual(left, right);
}

}  // namespace ruvia::detail
