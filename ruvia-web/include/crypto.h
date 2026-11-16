#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>

struct evp_md_ctx_st;

// OpenSSL-backed byte comparison, SHA-256, HMAC and CSPRNG primitives.
namespace ruvia {

// Length-checked, data-independent (constant-time) byte comparison for secrets
// and authentication tags. It never
// early-outs on the first differing byte, so the time taken does not leak how
// far an attacker-supplied value matched the expected one. Unequal lengths are
// reported immediately; that is not secret since these values are fixed-length.
[[nodiscard]] bool constant_time_bytes_equal(std::string_view left, std::string_view right) noexcept;

inline constexpr std::size_t sha256_digest_size = 32;
using sha256_digest = std::array<unsigned char, sha256_digest_size>;

// One-shot SHA-256 into `digest`. Returns false only when OpenSSL fails, so
// noexcept callers such as handshake callbacks can map it to their own failure.
[[nodiscard]] bool compute_sha256(std::span<const unsigned char> bytes, std::span<unsigned char, sha256_digest_size> digest) noexcept;

// SHA-256 over input fed in pieces. Input that fits the inline buffer is
// digested in one call; longer input streams through an OpenSSL context
// created once it overflows. Failures throw std::runtime_error.
class sha256 final {
public:
    sha256() noexcept = default;

    ~sha256();

    sha256(const sha256&) = delete;
    sha256& operator=(const sha256&) = delete;

    void update(std::string_view bytes);

    // Eight little-endian bytes.
    void update_u64_le(std::uint64_t value);

    // Ends the digest; nothing may be fed afterwards.
    [[nodiscard]] sha256_digest finish();

private:
    [[noreturn]] static void throw_digest_failure();

    std::array<unsigned char, 512> buffer_;
    std::size_t buffered_{0};
    ::evp_md_ctx_st* context_{nullptr};
};

[[nodiscard]] sha256_digest sha256_of(std::string_view bytes);

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

// Writes the hmac_digest_size(digest) bytes of HMAC(key, message) to the front
// of `output`. Returns false when key or message exceeds hmac_max_input_size,
// `output` is too small, or OpenSSL fails; callers check the sizes first to
// report them in their own terms.
[[nodiscard]] bool compute_hmac(hmac_digest digest, std::string_view key, std::string_view message, std::span<std::uint8_t> output) noexcept;

// Fills bytes from OpenSSL's CSPRNG. Returns false when the generator cannot
// produce output; callers translate that into their own failure contract.
[[nodiscard]] bool fill_random_bytes(std::span<unsigned char> bytes) noexcept;

}  // namespace ruvia
