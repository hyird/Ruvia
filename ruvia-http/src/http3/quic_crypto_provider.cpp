#include "ruvia/http/quic_crypto_provider.h"

#include <stdexcept>
#include <utility>

namespace ruvia {

quic_aead_key quic_aead_key::adopt(void* state_value, quic_aead_key_operations operations_value) {
    if (!state_value || !operations_value.destroy_ || !operations_value.seal_ || !operations_value.open_) {
        throw std::invalid_argument("invalid QUIC AEAD primitive ownership");
    }
    quic_aead_key key;
    key.state_ = state_value;
    key.operations_ = operations_value;
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
    operations_.seal_(state_, nonce, associated_data, plaintext, ciphertext_and_tag);
}

quic_aead_key_operations::open_result quic_aead_key::open(
    std::span<const std::byte, 12> nonce, std::span<const std::byte> associated_data,
    std::span<const std::byte> ciphertext_and_tag, std::span<std::byte> plaintext) const {
    if (!state_) {
        throw std::logic_error("cannot use an empty QUIC AEAD key");
    }
    const auto result_value = operations_.open_(state_, nonce, associated_data, ciphertext_and_tag, plaintext);
    if (result_value.value_ == quic_aead_key_operations::open_result::status::authenticated &&
        result_value.plaintext_size_ > plaintext.size()) {
        throw std::runtime_error("QUIC AEAD provider returned an invalid plaintext size");
    }
    return result_value;
}

void quic_aead_key::reset() noexcept {
    if (state_) {
        operations_.destroy_(state_);
        state_ = nullptr;
    }
    operations_ = {};
}

quic_header_protection_key quic_header_protection_key::adopt(
    void* state_value, quic_header_protection_key_operations operations_value) {
    if (!state_value || !operations_value.destroy_ || !operations_value.mask_) {
        throw std::invalid_argument("invalid QUIC header protection primitive ownership");
    }
    quic_header_protection_key key;
    key.state_ = state_value;
    key.operations_ = operations_value;
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
    operations_.mask_(state_, sample, output);
}

void quic_header_protection_key::reset() noexcept {
    if (state_) {
        operations_.destroy_(state_);
        state_ = nullptr;
    }
    operations_ = {};
}

void quic_crypto_provider_view::validate() const {
    if (!random_bytes_ || !hkdf_extract_ || !hkdf_expand_ || !create_aead_key_ ||
        !create_header_protection_key_ || !secure_erase_) {
        throw std::invalid_argument("QUIC crypto provider view is incomplete");
    }
}

}  // namespace ruvia
