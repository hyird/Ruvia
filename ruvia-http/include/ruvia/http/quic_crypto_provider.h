#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "ruvia/http/quic_types.h"

namespace ruvia {

struct quic_aead_key_operations {
    void (*destroy)(void* state) noexcept {};
    void (*seal)(void* state, std::span<const std::byte, 12> nonce,
        std::span<const std::byte> associated_data, std::span<const std::byte> plaintext,
        std::span<std::byte> ciphertext_and_tag){};
    // rejected is normal unauthenticated input; callers must ignore output bytes.
    struct open_result {
        enum class status : std::uint8_t { authenticated,
            rejected } value{status::rejected};
        std::size_t plaintext_size{};
    };
    open_result (*open)(void* state, std::span<const std::byte, 12> nonce,
        std::span<const std::byte> associated_data,
        std::span<const std::byte> ciphertext_and_tag, std::span<std::byte> plaintext){};
};

struct quic_header_protection_key_operations {
    void (*destroy)(void* state) noexcept {};
    void (*mask)(void* state, std::span<const std::byte, 16> sample, std::span<std::byte, 5> output){};
};

class quic_aead_key {
public:
    quic_aead_key() noexcept = default;
    quic_aead_key(const quic_aead_key&) = delete;
    quic_aead_key& operator=(const quic_aead_key&) = delete;
    quic_aead_key(quic_aead_key&& other) noexcept;
    quic_aead_key& operator=(quic_aead_key&& other) noexcept;
    ~quic_aead_key() noexcept;

    // On failure ownership remains with the caller; after success this key owns state.
    // Backends should keep reusable cipher contexts in state and erase key material in destroy.
    static quic_aead_key adopt(void* state, quic_aead_key_operations operations);

    explicit operator bool() const noexcept;
    void seal(std::span<const std::byte, 12> nonce, std::span<const std::byte> associated_data,
        std::span<const std::byte> plaintext, std::span<std::byte> ciphertext_and_tag) const;
    quic_aead_key_operations::open_result open(
        std::span<const std::byte, 12> nonce, std::span<const std::byte> associated_data,
        std::span<const std::byte> ciphertext_and_tag, std::span<std::byte> plaintext) const;

private:
    void reset() noexcept;

    void* state_{};
    quic_aead_key_operations operations_{};
};

class quic_header_protection_key {
public:
    quic_header_protection_key() noexcept = default;
    quic_header_protection_key(const quic_header_protection_key&) = delete;
    quic_header_protection_key& operator=(const quic_header_protection_key&) = delete;
    quic_header_protection_key(quic_header_protection_key&& other) noexcept;
    quic_header_protection_key& operator=(quic_header_protection_key&& other) noexcept;
    ~quic_header_protection_key() noexcept;

    // On failure ownership remains with the caller; after success this key owns state.
    static quic_header_protection_key adopt(void* state, quic_header_protection_key_operations operations);

    explicit operator bool() const noexcept;
    void mask(std::span<const std::byte, 16> sample, std::span<std::byte, 5> output) const;

private:
    void reset() noexcept;

    void* state_{};
    quic_header_protection_key_operations operations_{};
};

// A synchronous backend view, borrowed by an HTTP connection/server. validate() is
// called once when that owner is constructed, never on individual crypto operations.
// Creator callbacks return typed owners. Provider exceptions may cross C++ protocol
// calls, but C ABI callback adapters must catch and latch them for outer rethrow.
struct quic_crypto_provider_view {
    void* context{};
    void (*random_bytes)(void* context, std::span<std::byte> output){};
    void (*hkdf_extract)(void* context, quic_cipher_suite suite, std::span<const std::byte> salt,
        std::span<const std::byte> input_key_material, std::span<std::byte> output){};
    void (*hkdf_expand)(void* context, quic_cipher_suite suite, std::span<const std::byte> secret,
        std::span<const std::byte> info, std::span<std::byte> output){};
    quic_aead_key (*create_aead_key)(void* context, quic_cipher_suite suite,
        quic_crypto_direction direction, std::span<const std::byte> key){};
    quic_header_protection_key (*create_header_protection_key)(
        void* context, quic_cipher_suite suite, std::span<const std::byte> key){};
    void (*secure_erase)(void* context, std::span<std::byte> bytes) noexcept {};

    void validate() const;
};

}  // namespace ruvia
