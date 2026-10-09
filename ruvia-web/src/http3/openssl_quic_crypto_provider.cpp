#include "http3/openssl_quic_crypto_provider.h"

#include <algorithm>
#include <array>
#include <climits>
#include <cstring>
#include <memory>
#include <new>
#include <stdexcept>

#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/params.h>
#include <openssl/rand.h>

namespace ruvia::detail {
namespace {

struct cipher_spec {
    const char* aead;
    const char* header_protection;
    const char* digest;
    std::size_t key_size;
};

cipher_spec spec_for(quic_cipher_suite suite) {
    switch (suite) {
        case quic_cipher_suite::aes_128_gcm_sha256:
            return {"AES-128-GCM", "AES-128-ECB", "SHA256", 16};
        case quic_cipher_suite::aes_256_gcm_sha384:
            return {"AES-256-GCM", "AES-256-ECB", "SHA384", 32};
        case quic_cipher_suite::chacha20_poly1305_sha256:
            return {"CHACHA20-POLY1305", "CHACHA20", "SHA256", 32};
    }
    throw std::invalid_argument("unsupported QUIC TLS cipher suite");
}

void derive_hkdf(EVP_KDF* algorithm, int mode, const char* digest,
    std::span<const std::byte> salt, std::span<const std::byte> key,
    std::span<const std::byte> info, std::span<std::byte> output) {
    std::unique_ptr<EVP_KDF_CTX, decltype(&EVP_KDF_CTX_free)> context(
        EVP_KDF_CTX_new(algorithm), EVP_KDF_CTX_free);
    if (!context) {
        throw std::runtime_error("OpenSSL HKDF context allocation failed");
    }
    std::array<char, 7> digest_name{};
    std::memcpy(digest_name.data(), digest, std::strlen(digest));
    std::byte empty_input{};
    const auto octets = [&empty_input](const char* name, std::span<const std::byte> bytes) {
        // OSSL_PARAM requires non-null storage even for an empty octet string.
        auto* data = bytes.empty() ? &empty_input : const_cast<std::byte*>(bytes.data());
        return OSSL_PARAM_construct_octet_string(name, data, bytes.size());
    };
    const OSSL_PARAM parameters[]{
        OSSL_PARAM_construct_int(OSSL_KDF_PARAM_MODE, &mode),
        OSSL_PARAM_construct_utf8_string(OSSL_KDF_PARAM_DIGEST, digest_name.data(), 0),
        octets(OSSL_KDF_PARAM_KEY, key),
        octets(OSSL_KDF_PARAM_SALT, salt),
        octets(OSSL_KDF_PARAM_INFO, info),
        OSSL_PARAM_construct_end()};
    if (EVP_KDF_derive(context.get(), reinterpret_cast<unsigned char*>(output.data()),
            output.size(), parameters) != 1) {
        throw std::runtime_error("OpenSSL HKDF derivation failed");
    }
}

struct key_state {
    std::pmr::memory_resource* resource;
    const EVP_CIPHER* cipher;
    EVP_CIPHER_CTX* context;
    std::array<unsigned char, 32> key{};
    bool header_protection;
};

void erase(void* opaque) noexcept {
    auto* state = static_cast<key_state*>(opaque);
    if (!state) {
        return;
    }
    EVP_CIPHER_CTX_free(state->context);
    OPENSSL_cleanse(state->key.data(), state->key.size());
    auto* resource = state->resource;
    state->~key_state();
    resource->deallocate(state, sizeof(key_state), alignof(key_state));
}

void aead_seal(void* opaque, std::span<const std::byte, 12> nonce,
    std::span<const std::byte> aad, std::span<const std::byte> plaintext,
    std::span<std::byte> output) {
    auto& state = *static_cast<key_state*>(opaque);
    if (plaintext.size() > INT_MAX || aad.size() > INT_MAX ||
        output.size() < plaintext.size() + 16) {
        throw std::invalid_argument("invalid QUIC AEAD output or input size");
    }
    int aad_length{};
    int payload_length{};
    int final_length{};
    OSSL_PARAM tag_parameters[]{
        OSSL_PARAM_construct_octet_string(OSSL_CIPHER_PARAM_AEAD_TAG,
            output.data() + plaintext.size(), 16),
        OSSL_PARAM_construct_end()};
    if (EVP_EncryptInit_ex2(state.context, state.cipher, state.key.data(),
            reinterpret_cast<const unsigned char*>(nonce.data()), nullptr) != 1 ||
        (!aad.empty() && EVP_EncryptUpdate(state.context, nullptr, &aad_length,
                             reinterpret_cast<const unsigned char*>(aad.data()), static_cast<int>(aad.size())) != 1) ||
        (!plaintext.empty() && EVP_EncryptUpdate(state.context,
                                   reinterpret_cast<unsigned char*>(output.data()), &payload_length,
                                   reinterpret_cast<const unsigned char*>(plaintext.data()), static_cast<int>(plaintext.size())) != 1)) {
        throw std::runtime_error("OpenSSL QUIC AEAD encryption failed");
    }
    if (payload_length < 0 || static_cast<std::size_t>(payload_length) != plaintext.size() ||
        EVP_EncryptFinal_ex(state.context,
            reinterpret_cast<unsigned char*>(output.data()) + payload_length, &final_length) != 1 ||
        final_length != 0 ||
        EVP_CIPHER_CTX_get_params(state.context, tag_parameters) != 1) {
        throw std::runtime_error("OpenSSL QUIC AEAD finalization failed");
    }
}

quic_aead_key_operations::open_result aead_open(void* opaque,
    std::span<const std::byte, 12> nonce, std::span<const std::byte> aad,
    std::span<const std::byte> input, std::span<std::byte> output) {
    using result = quic_aead_key_operations::open_result;
    auto& state = *static_cast<key_state*>(opaque);
    if (input.size() < 16) {
        return {result::status::rejected, 0};
    }
    const auto data_size = input.size() - 16;
    if (data_size > output.size() || aad.size() > INT_MAX || input.size() > INT_MAX) {
        throw std::invalid_argument("invalid QUIC AEAD output or input size");
    }
    int aad_length{};
    int payload_length{};
    int final_length{};
    if (EVP_DecryptInit_ex2(state.context, state.cipher, state.key.data(),
            reinterpret_cast<const unsigned char*>(nonce.data()), nullptr) != 1 ||
        (!aad.empty() && EVP_DecryptUpdate(state.context, nullptr, &aad_length,
                             reinterpret_cast<const unsigned char*>(aad.data()), static_cast<int>(aad.size())) != 1) ||
        (data_size != 0 && EVP_DecryptUpdate(state.context, reinterpret_cast<unsigned char*>(output.data()),
                               &payload_length, reinterpret_cast<const unsigned char*>(input.data()), static_cast<int>(data_size)) != 1)) {
        throw std::runtime_error("OpenSSL QUIC AEAD decryption failed");
    }
    std::array<unsigned char, 16> tag{};
    std::memcpy(tag.data(), input.data() + data_size, tag.size());
    const OSSL_PARAM tag_parameters[]{
        OSSL_PARAM_construct_octet_string(OSSL_CIPHER_PARAM_AEAD_TAG, tag.data(), tag.size()),
        OSSL_PARAM_construct_end()};
    if (EVP_CIPHER_CTX_set_params(state.context, tag_parameters) != 1) {
        throw std::runtime_error("OpenSSL QUIC AEAD tag setup failed");
    }
    std::array<unsigned char, 16> final_buffer{};
    auto* const final_output = output.empty()
                                   ? final_buffer.data()
                                   : reinterpret_cast<unsigned char*>(output.data()) + payload_length;
    if (EVP_DecryptFinal_ex(state.context, final_output, &final_length) != 1) {
        if (data_size != 0) {
            OPENSSL_cleanse(output.data(), data_size);
        }
        return {result::status::rejected, 0};
    }
    if (payload_length < 0 || final_length < 0 ||
        static_cast<std::size_t>(payload_length + final_length) != data_size) {
        throw std::runtime_error("OpenSSL QUIC AEAD plaintext length mismatch");
    }
    return {result::status::authenticated, static_cast<std::size_t>(payload_length + final_length)};
}

void hp_mask(void* opaque, std::span<const std::byte, 16> sample, std::span<std::byte, 5> output) {
    auto& state = *static_cast<key_state*>(opaque);
    std::array<unsigned char, 16> block{};
    int size{};
    std::array<unsigned char, 16> iv{};
    const unsigned char* iv_ptr = nullptr;
    const bool chacha = EVP_CIPHER_is_a(state.cipher, "CHACHA20");
    if (chacha) {
        std::memcpy(iv.data(), sample.data(), sample.size());
        iv_ptr = iv.data();
    }
    std::array<unsigned char, 16> zeroes{};
    if (EVP_EncryptInit_ex2(state.context, state.cipher, state.key.data(), iv_ptr, nullptr) != 1 ||
        EVP_EncryptUpdate(state.context, block.data(), &size,
            chacha ? zeroes.data() : reinterpret_cast<const unsigned char*>(sample.data()),
            chacha ? 5 : static_cast<int>(sample.size())) != 1 ||
        size < 5) {
        throw std::runtime_error("OpenSSL QUIC header protection failed");
    }
    std::memcpy(output.data(), block.data(), output.size());
    OPENSSL_cleanse(block.data(), block.size());
}

}  // namespace

struct openssl_quic_crypto_provider::impl {
    explicit impl(std::pmr::memory_resource* memory)
        : resource(memory ? memory : std::pmr::get_default_resource()) {
        for (const auto suite : {quic_cipher_suite::aes_128_gcm_sha256,
                 quic_cipher_suite::aes_256_gcm_sha384, quic_cipher_suite::chacha20_poly1305_sha256}) {
            const auto spec = spec_for(suite);
            auto& slot = entries[index(suite)];
            slot.aead.reset(EVP_CIPHER_fetch(nullptr, spec.aead, nullptr));
            slot.hp.reset(EVP_CIPHER_fetch(nullptr, spec.header_protection, nullptr));
            if (!slot.aead || !slot.hp) {
                throw std::runtime_error("OpenSSL QUIC algorithm fetch failed");
            }
        }
        hkdf.reset(EVP_KDF_fetch(nullptr, "HKDF", nullptr));
        if (!hkdf) {
            throw std::runtime_error("OpenSSL QUIC HKDF fetch failed");
        }
    }
    struct entry {
        std::unique_ptr<EVP_CIPHER, decltype(&EVP_CIPHER_free)> aead{nullptr, EVP_CIPHER_free};
        std::unique_ptr<EVP_CIPHER, decltype(&EVP_CIPHER_free)> hp{nullptr, EVP_CIPHER_free};
    };
    static std::size_t index(quic_cipher_suite suite) {
        switch (suite) {
            case quic_cipher_suite::aes_128_gcm_sha256:
                return 0;
            case quic_cipher_suite::aes_256_gcm_sha384:
                return 1;
            case quic_cipher_suite::chacha20_poly1305_sha256:
                return 2;
        }
        throw std::invalid_argument("unsupported QUIC TLS cipher suite");
    }
    std::pmr::memory_resource* resource;
    std::array<entry, 3> entries;
    std::unique_ptr<EVP_KDF, decltype(&EVP_KDF_free)> hkdf{nullptr, EVP_KDF_free};
};

openssl_quic_crypto_provider::openssl_quic_crypto_provider(std::pmr::memory_resource* resource) {
    auto* memory = resource ? resource : std::pmr::get_default_resource();
    void* const storage = memory->allocate(sizeof(impl), alignof(impl));
    try {
        impl_ = new (storage) impl(memory);
    } catch (...) {
        memory->deallocate(storage, sizeof(impl), alignof(impl));
        throw;
    }
}

openssl_quic_crypto_provider::~openssl_quic_crypto_provider() noexcept {
    if (!impl_) {
        return;
    }
    auto* resource = impl_->resource;
    impl_->~impl();
    resource->deallocate(impl_, sizeof(impl), alignof(impl));
}

quic_crypto_provider_view openssl_quic_crypto_provider::view() noexcept {
    return {impl_,
        [](void*, std::span<std::byte> output) {
            if (output.size() > INT_MAX || RAND_bytes_ex(nullptr, reinterpret_cast<unsigned char*>(output.data()), output.size(), 0) != 1) {
                throw std::runtime_error("OpenSSL QUIC random generation failed");
            }
        },
        [](void* opaque, quic_cipher_suite suite, std::span<const std::byte> salt,
            std::span<const std::byte> ikm, std::span<std::byte> output) {
            auto& self = *static_cast<impl*>(opaque);
            if (salt.size() > INT_MAX || ikm.size() > INT_MAX || output.size() > INT_MAX) {
                throw std::invalid_argument("QUIC HKDF input exceeds OpenSSL limits");
            }
            derive_hkdf(self.hkdf.get(), EVP_KDF_HKDF_MODE_EXTRACT_ONLY,
                spec_for(suite).digest, salt, ikm, {}, output);
        },
        [](void* opaque, quic_cipher_suite suite, std::span<const std::byte> secret,
            std::span<const std::byte> info, std::span<std::byte> output) {
            auto& self = *static_cast<impl*>(opaque);
            if (secret.size() > INT_MAX || info.size() > INT_MAX || output.size() > INT_MAX) {
                throw std::invalid_argument("QUIC HKDF input exceeds OpenSSL limits");
            }
            derive_hkdf(self.hkdf.get(), EVP_KDF_HKDF_MODE_EXPAND_ONLY,
                spec_for(suite).digest, {}, secret, info, output);
        },
        [](void* opaque, quic_cipher_suite suite, quic_crypto_direction,
            std::span<const std::byte> key) {
            auto& self = *static_cast<impl*>(opaque);
            const auto spec = spec_for(suite);
            if (key.size() != spec.key_size) {
                throw std::invalid_argument("invalid QUIC AEAD key size");
            }
            auto* resource = self.resource;
            auto* state = new (resource->allocate(sizeof(key_state), alignof(key_state)))
                key_state{resource, self.entries[impl::index(suite)].aead.get(), EVP_CIPHER_CTX_new(), {}, false};
            if (!state->context) {
                erase(state);
                throw std::bad_alloc();
            }
            std::memcpy(state->key.data(), key.data(), key.size());
            try {
                return quic_aead_key::adopt(state, {erase, aead_seal, aead_open});
            } catch (...) {
                erase(state);
                throw;
            }
        },
        [](void* opaque, quic_cipher_suite suite, std::span<const std::byte> key) {
            auto& self = *static_cast<impl*>(opaque);
            const auto spec = spec_for(suite);
            if (key.size() != spec.key_size) {
                throw std::invalid_argument("invalid QUIC header protection key size");
            }
            auto* resource = self.resource;
            auto* state = new (resource->allocate(sizeof(key_state), alignof(key_state)))
                key_state{resource, self.entries[impl::index(suite)].hp.get(), EVP_CIPHER_CTX_new(), {}, true};
            if (!state->context) {
                erase(state);
                throw std::bad_alloc();
            }
            std::memcpy(state->key.data(), key.data(), key.size());
            try {
                return quic_header_protection_key::adopt(state, {erase, hp_mask});
            } catch (...) {
                erase(state);
                throw;
            }
        },
        [](void*, std::span<std::byte> bytes) noexcept { OPENSSL_cleanse(bytes.data(), bytes.size()); }};
}

}  // namespace ruvia::detail
