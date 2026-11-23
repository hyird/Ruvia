#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "byte_view.h"
#include "lifetimebound.h"

struct evp_md_ctx_st;

// OpenSSL-backed constant-time comparison, SHA-256, HMAC, CSPRNG and signed-cookie primitives.
namespace ruvia {

// Length-checked, data-independent (constant-time) byte comparison for secrets
// and authentication tags.
[[nodiscard]] bool constant_time_bytes_equal(byte_view left, byte_view right) noexcept;

[[nodiscard]] inline bool constant_time_bytes_equal(std::string_view left, std::string_view right) noexcept {
    return constant_time_bytes_equal(as_bytes(left), as_bytes(right));
}

inline constexpr std::size_t sha256_digest_size = 32;
using sha256_digest = std::array<std::byte, sha256_digest_size>;

// One-shot SHA-256 into `digest`. Returns false only when OpenSSL fails.
[[nodiscard]] bool compute_sha256(byte_view bytes, std::span<std::byte, sha256_digest_size> digest) noexcept;

// SHA-256 over input fed in pieces. Input that fits the inline buffer is
// digested in one call; longer input streams through an OpenSSL context
// created once it overflows. Failures throw std::runtime_error.
class sha256 final {
public:
    sha256() noexcept = default;
    ~sha256();

    sha256(const sha256&) = delete;
    sha256& operator=(const sha256&) = delete;

    void update(byte_view bytes);

    void update(std::string_view bytes) {
        update(as_bytes(bytes));
    }

    // Eight little-endian bytes.
    void update_u64_le(std::uint64_t value);

    // Ends the digest; nothing may be fed afterwards until reset().
    [[nodiscard]] sha256_digest finish();

    // Resets the hasher to active state so the underlying digest context can be reused.
    void reset() noexcept;

private:
    enum class state_type : std::uint8_t {
        active,
        finished,
        failed,
    };

    [[noreturn]] static void throw_digest_failure();

    std::array<std::byte, 512> buffer_{};
    std::size_t buffered_{0};
    ::evp_md_ctx_st* context_{nullptr};
    state_type state_{state_type::active};
};

[[nodiscard]] sha256_digest sha256_of(byte_view bytes);

[[nodiscard]] inline sha256_digest sha256_of(std::string_view bytes) {
    return sha256_of(as_bytes(bytes));
}

// The digests behind signed cookies (SHA-256) and JWT HS256/HS384/HS512.
enum class hmac_digest : std::uint8_t {
    sha256,
    sha384,
    sha512,
};

inline constexpr std::size_t hmac_max_digest_size = 64;
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

// Writes the hmac_digest_size(digest) bytes of HMAC(key, message) to the front
// of `output`. Returns false when key or message exceeds hmac_max_input_size,
// `output` is too small, or OpenSSL fails.
[[nodiscard]] bool compute_hmac(hmac_digest digest, byte_view key, byte_view message, mutable_byte_view output) noexcept;

[[nodiscard]] inline bool compute_hmac(hmac_digest digest, std::string_view key, std::string_view message, mutable_byte_view output) noexcept {
    return compute_hmac(digest, as_bytes(key), as_bytes(message), output);
}

// Fills bytes from OpenSSL's CSPRNG. Returns false when the generator fails.
[[nodiscard]] bool fill_random_bytes(mutable_byte_view bytes) noexcept;

// Base64(HMAC-SHA256) character width of a signed cookie signature.
inline constexpr std::size_t cookie_signature_size = 44;

// Writes the 44-character Base64(HMAC-SHA256) signature of length-framed
// `(name, value)` under `secret` into `output`.
void write_cookie_signature(std::span<char, cookie_signature_size> output, std::string_view secret, std::string_view name, std::string_view value);

// Formats `"<value>.<signature>"` bound to cookie `name` under `secret`.
[[nodiscard]] std::string sign_cookie_value(std::string_view secret, std::string_view name, std::string_view value);

// Verifies `"<value>.<signature>"` in constant time and returns `<value>`
// borrowing `signed_value`, or `std::nullopt` on mismatch/malformed input.
[[nodiscard]] std::optional<std::string_view> verify_signed_cookie_value(std::string_view secret, std::string_view name, std::string_view signed_value RUVIA_LIFETIMEBOUND);
std::optional<std::string_view> verify_signed_cookie_value(std::string_view, std::string_view, std::string&&) = delete;
std::optional<std::string_view> verify_signed_cookie_value(std::string_view, std::string_view, const std::string&&) = delete;

}  // namespace ruvia
