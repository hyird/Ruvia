#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

struct evp_md_ctx_st;

namespace ruvia::detail {

// SHA-256 over input fed in pieces, the one digest the database layer keys
// statements, cache entries and migration history by. Input that fits the
// inline buffer is digested in one call; longer input streams through an
// OpenSSL context created once it overflows.
class db_sha256 final {
public:
    static constexpr std::size_t digest_size = 32;
    using digest_type = std::array<unsigned char, digest_size>;

    db_sha256() noexcept = default;
    ~db_sha256();

    db_sha256(const db_sha256&) = delete;
    db_sha256& operator=(const db_sha256&) = delete;

    void update(std::string_view bytes);
    // Eight little-endian bytes.
    void update_u64_le(std::uint64_t value);
    // Ends the digest; nothing may be fed afterwards.
    [[nodiscard]] digest_type finish();

private:
    std::array<unsigned char, 512> buffer_;
    std::size_t buffered_{0};
    evp_md_ctx_st* context_{nullptr};
};

[[nodiscard]] inline db_sha256::digest_type db_sha256_of(std::string_view bytes) {
    db_sha256 digest;
    digest.update(bytes);
    return digest.finish();
}

}  // namespace ruvia::detail
