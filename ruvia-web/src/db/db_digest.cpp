#include "db/db_digest.h"

#include <cstring>
#include <stdexcept>

#include <openssl/evp.h>
#include <openssl/sha.h>

namespace ruvia::detail {
namespace {

static_assert(db_sha256::digest_size == SHA256_DIGEST_LENGTH);

[[noreturn]] void throw_digest_failure() {
    throw std::runtime_error("database SHA-256 digest failed");
}

}  // namespace

db_sha256::~db_sha256() {
    if (context_ != nullptr) {
        EVP_MD_CTX_free(context_);
    }
}

void db_sha256::update(std::string_view bytes) {
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

void db_sha256::update_u64_le(std::uint64_t value) {
    std::array<char, 8> encoded;
    for (unsigned index = 0; index < encoded.size(); ++index) {
        encoded[index] = static_cast<char>((value >> (index * 8)) & 255);
    }
    update({encoded.data(), encoded.size()});
}

db_sha256::digest_type db_sha256::finish() {
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

}  // namespace ruvia::detail
