#include "tls/tls_session_ticket_keys.h"

#include <algorithm>
#include <stdexcept>

#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/params.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>

#include "tls/tls_random.h"

namespace ruvia::detail {
namespace {

[[nodiscard]] std::optional<tls_session_ticket_key> generate_ticket_key() noexcept {
    tls_session_ticket_key key;
    if (!fill_random_bytes(key.name_) || !fill_random_bytes(key.hmac_key_) || !fill_random_bytes(key.aes_key_)) {
        return std::nullopt;
    }
    return key;
}

// The ex_data slot that points every TLS context of a listener at its
// worker's key set. Allocated by the first install(), at startup.
[[nodiscard]] int ticket_key_set_index() noexcept {
    static const int index = SSL_CTX_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr);
    return index;
}

[[nodiscard]] bool set_ticket_mac_key(EVP_MAC_CTX& mac, const tls_session_ticket_key& key) noexcept {
    static char digest[] = "SHA256";
    const OSSL_PARAM parameters[] = {
        OSSL_PARAM_construct_octet_string(
            OSSL_MAC_PARAM_KEY, const_cast<unsigned char*>(key.hmac_key_.data()), key.hmac_key_.size()),
        OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_DIGEST, digest, 0),
        OSSL_PARAM_construct_end(),
    };
    return EVP_MAC_CTX_set_params(&mac, parameters) == 1;
}

[[nodiscard]] bool names_key(const std::optional<tls_session_ticket_key>& key, const unsigned char* name) noexcept {
    return key.has_value() && std::equal(key->name_.begin(), key->name_.end(), name);
}

}  // namespace

tls_session_ticket_key::~tls_session_ticket_key() {
    OPENSSL_cleanse(name_.data(), name_.size());
    OPENSSL_cleanse(hmac_key_.data(), hmac_key_.size());
    OPENSSL_cleanse(aes_key_.data(), aes_key_.size());
}

tls_session_ticket_key_ring::tls_session_ticket_key_ring(std::chrono::steady_clock::time_point origin)
    : origin_(origin),
      current_(generate_ticket_key()) {
    if (!current_.has_value()) {
        throw std::runtime_error("failed to generate TLS session ticket keys");
    }
}

std::uint64_t tls_session_ticket_key_ring::epoch_at(std::chrono::steady_clock::time_point now) const noexcept {
    if (now <= origin_) {
        return 0;
    }
    return static_cast<std::uint64_t>((now - origin_) / tls_session_lifetime);
}

std::chrono::steady_clock::time_point tls_session_ticket_key_ring::next_rotation(
    std::chrono::steady_clock::time_point now) const noexcept {
    return origin_ + (epoch_at(now) + 1) * std::chrono::duration_cast<std::chrono::steady_clock::duration>(tls_session_lifetime);
}

bool tls_session_ticket_key_ring::copy_to(std::chrono::steady_clock::time_point now, tls_session_ticket_key_set& target) {
    const std::lock_guard lock(mutex_);
    const auto epoch = epoch_at(now);
    if (epoch > epoch_) {
        // The previous key's validity ended with the old epoch; the current
        // key stays valid for decryption through exactly one more epoch.
        previous_.reset();
        if (epoch == epoch_ + 1) {
            previous_ = current_;
        }
        current_.reset();
        epoch_ = epoch;
    }
    if (!current_.has_value()) {
        current_ = generate_ticket_key();
    }
    target.current_ = current_;
    target.previous_ = previous_;
    return current_.has_value();
}

tls_session_ticket_key_set::tls_session_ticket_key_set()
    : aes_256_cbc_(EVP_CIPHER_fetch(nullptr, "AES-256-CBC", nullptr)) {
    if (aes_256_cbc_ == nullptr) {
        throw std::runtime_error("failed to fetch the TLS session ticket cipher");
    }
}

tls_session_ticket_key_set::~tls_session_ticket_key_set() {
    EVP_CIPHER_free(aes_256_cbc_);
}

void tls_session_ticket_key_set::install(SSL_CTX& context) {
    const int index = ticket_key_set_index();
    if (index < 0 || SSL_CTX_set_ex_data(&context, index, this) != 1 ||
        SSL_CTX_set_tlsext_ticket_key_evp_cb(&context, &tls_session_ticket_key_set::protect_ticket) != 1) {
        throw std::runtime_error("failed to install TLS session ticket keys");
    }
    (void)SSL_CTX_set_timeout(&context, static_cast<long>(tls_session_lifetime.count()));
}

// SSL_CTX_set_tlsext_ticket_key_evp_cb(3): returns 1 when the key is set up,
// 2 when a ticket decrypted with the previous key should be renewed, 0 when no
// ticket is issued or the key name is unknown (full handshake), -1 on error.
int tls_session_ticket_key_set::protect_ticket(SSL* ssl, unsigned char* key_name, unsigned char* iv,
    EVP_CIPHER_CTX* cipher, EVP_MAC_CTX* mac, int encrypt) noexcept {
    // OpenSSL calls the callback of the connection's initial context, but an
    // SNI switch may already have replaced the context; every context of the
    // listener points at the same set.
    const auto* keys = static_cast<const tls_session_ticket_key_set*>(
        SSL_CTX_get_ex_data(SSL_get_SSL_CTX(ssl), ticket_key_set_index()));
    if (keys == nullptr || cipher == nullptr || mac == nullptr) {
        return -1;
    }
    if (encrypt != 0) {
        if (!keys->current_.has_value()) {
            return 0;
        }
        const auto& key = *keys->current_;
        std::ranges::copy(key.name_, key_name);
        const int iv_size = EVP_CIPHER_get_iv_length(keys->aes_256_cbc_);
        if (iv_size <= 0 || iv_size > EVP_MAX_IV_LENGTH || RAND_bytes(iv, iv_size) != 1 ||
            EVP_EncryptInit_ex2(cipher, keys->aes_256_cbc_, key.aes_key_.data(), iv, nullptr) != 1 ||
            !set_ticket_mac_key(*mac, key)) {
            return -1;
        }
        return 1;
    }
    const bool current = names_key(keys->current_, key_name);
    if (!current && !names_key(keys->previous_, key_name)) {
        return 0;
    }
    const auto& key = current ? *keys->current_ : *keys->previous_;
    if (!set_ticket_mac_key(*mac, key) ||
        EVP_DecryptInit_ex2(cipher, keys->aes_256_cbc_, key.aes_key_.data(), iv, nullptr) != 1) {
        return -1;
    }
    return current ? 1 : 2;
}

}  // namespace ruvia::detail
