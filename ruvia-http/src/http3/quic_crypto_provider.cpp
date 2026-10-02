#include "ruvia/http/quic_crypto_provider.h"

#include <stdexcept>
#include <utility>

namespace ruvia {

quic_aead_key quic_aead_key::adopt(void* state, quic_aead_key_operations operations) {
    if (!state || !operations.destroy || !operations.seal || !operations.open) {
        throw std::invalid_argument("invalid QUIC AEAD primitive ownership");
    }
    quic_aead_key key;
    key.state_ = state;
    key.operations_ = operations;
    return key;
}

quic_aead_key::quic_aead_key(quic_aead_key&& other) noexcept
    : state_(std::exchange(other.state_, nullptr)),
      operations_(std::exchange(other.operations_, {})) {}

quic_aead_key& quic_aead_key::operator=(quic_aead_key&& other) noexcept {
    if (this != &other) {
        reset();
        state_ = std::exchange(other.state_, nullptr);
        operations_ = std::exchange(other.operations_, {});
    }
    return *this;
}

quic_aead_key::~quic_aead_key() noexcept {
    reset();
}

quic_aead_key::operator bool() const noexcept {
    return state_ != nullptr;
}

void quic_aead_key::seal(std::span<const std::byte, 12> nonce,
    std::span<const std::byte> associated_data,
    std::span<const std::byte> plaintext,
    std::span<std::byte> ciphertext_and_tag) const {
    if (!state_) {
        throw std::logic_error("cannot use an empty QUIC AEAD key");
    }
    operations_.seal(state_, nonce, associated_data, plaintext, ciphertext_and_tag);
}

quic_aead_key_operations::open_result quic_aead_key::open(
    std::span<const std::byte, 12> nonce, std::span<const std::byte> associated_data,
    std::span<const std::byte> ciphertext_and_tag, std::span<std::byte> plaintext) const {
    if (!state_) {
        throw std::logic_error("cannot use an empty QUIC AEAD key");
    }
    const auto result = operations_.open(state_, nonce, associated_data, ciphertext_and_tag, plaintext);
    if (result.value == quic_aead_key_operations::open_result::status::authenticated &&
        result.plaintext_size > plaintext.size()) {
        throw std::runtime_error("QUIC AEAD provider returned an invalid plaintext size");
    }
    return result;
}

void quic_aead_key::reset() noexcept {
    if (state_) {
        operations_.destroy(state_);
        state_ = nullptr;
    }
    operations_ = {};
}

quic_header_protection_key quic_header_protection_key::adopt(
    void* state, quic_header_protection_key_operations operations) {
    if (!state || !operations.destroy || !operations.mask) {
        throw std::invalid_argument("invalid QUIC header protection primitive ownership");
    }
    quic_header_protection_key key;
    key.state_ = state;
    key.operations_ = operations;
    return key;
}

quic_header_protection_key::quic_header_protection_key(quic_header_protection_key&& other) noexcept
    : state_(std::exchange(other.state_, nullptr)),
      operations_(std::exchange(other.operations_, {})) {}

quic_header_protection_key& quic_header_protection_key::operator=(quic_header_protection_key&& other) noexcept {
    if (this != &other) {
        reset();
        state_ = std::exchange(other.state_, nullptr);
        operations_ = std::exchange(other.operations_, {});
    }
    return *this;
}

quic_header_protection_key::~quic_header_protection_key() noexcept {
    reset();
}

quic_header_protection_key::operator bool() const noexcept {
    return state_ != nullptr;
}

void quic_header_protection_key::mask(std::span<const std::byte, 16> sample,
    std::span<std::byte, 5> output) const {
    if (!state_) {
        throw std::logic_error("cannot use an empty QUIC header protection key");
    }
    operations_.mask(state_, sample, output);
}

void quic_header_protection_key::reset() noexcept {
    if (state_) {
        operations_.destroy(state_);
        state_ = nullptr;
    }
    operations_ = {};
}

void quic_crypto_provider_view::validate() const {
    if (!random_bytes || !hkdf_extract || !hkdf_expand || !create_aead_key ||
        !create_header_protection_key || !secure_erase) {
        throw std::invalid_argument("QUIC crypto provider view is incomplete");
    }
}

}  // namespace ruvia
