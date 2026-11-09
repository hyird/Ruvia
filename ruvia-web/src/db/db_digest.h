#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string_view>

#include <openssl/evp.h>
#include <openssl/sha.h>

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
    static_assert(digest_size == SHA256_DIGEST_LENGTH);

    ~db_sha256() {
        if (context_ != nullptr) {
            EVP_MD_CTX_free(context_);
        }
    }

    db_sha256(const db_sha256&) = delete;
    db_sha256& operator=(const db_sha256&) = delete;

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
            if (context_ == nullptr || EVP_DigestInit_ex(context_, EVP_sha256(), nullptr) != 1 ||
                (buffered_ != 0 && EVP_DigestUpdate(context_, buffer_.data(), buffered_) != 1)) {
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
    [[nodiscard]] digest_type finish() {
        digest_type digest;
        if (context_ == nullptr) {
            if (SHA256(buffer_.data(), buffered_, digest.data()) == nullptr) {
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
        throw std::runtime_error("database SHA-256 digest failed");
    }

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
