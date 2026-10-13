#include "auth/cookie_signature.h"

#include <array>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <span>
#include <stdexcept>
#include <string>

#include "ruvia/http/http_base64.h"

#include "auth/hmac.h"

namespace ruvia::detail {

static_assert(cookie_signature_size == http_base64_encoded_size(hmac_digest_size(hmac_digest::sha256)));

void write_cookie_signature(
    char* output, std::string_view secret, std::string_view name, std::string_view value,
    std::pmr::memory_resource* resource) {
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
    // Assemble lenPrefix||name||value for the one-shot HMAC. A stack arena keeps
    // signing a typical cookie allocation-free; an oversized name/value spills to
    // the request's recyclable pool.
    if (name.size() > std::numeric_limits<std::size_t>::max() - name_len_bytes.size() ||
        value.size() >
            std::numeric_limits<std::size_t>::max() - name_len_bytes.size() - name.size()) {
        throw std::length_error("signed cookie message is too large");
    }
    std::array<std::byte, 512> message_buffer;
    std::pmr::monotonic_buffer_resource message_arena(message_buffer.data(), message_buffer.size(), resource);
    std::pmr::string message(&message_arena);
    const auto message_size = name_len_bytes.size() + name.size() + value.size();
    if (message_size > hmac_max_input_size) {
        throw std::length_error("signed cookie message is too large for HMAC");
    }
    message.reserve(message_size);
    message.append(name_len_bytes.data(), name_len_bytes.size());
    message.append(name.data(), name.size());
    message.append(value.data(), value.size());

    std::array<std::uint8_t, hmac_digest_size(hmac_digest::sha256)> digest{};
    if (!compute_hmac(hmac_digest::sha256, secret, message, digest)) {
        throw std::runtime_error("signed cookie HMAC failed");
    }
    http_base64_encode(output, std::span<const std::uint8_t>(digest));
}

}  // namespace ruvia::detail
