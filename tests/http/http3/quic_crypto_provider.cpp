#include "ruvia/http/quic_crypto_provider.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>

#include "test_harness.h"

namespace {

struct allocation_counts {
    std::size_t allocated_{};
    std::size_t released_{};
};

struct primitive_state {
    explicit primitive_state(allocation_counts* owner_value)
        : counts_(owner_value) {}
    allocation_counts* counts_{};
    ~primitive_state() {
        ++counts_->released_;
    }
};

void destroy_primitive(void* opaque) noexcept {
    std::unique_ptr<primitive_state> state(static_cast<primitive_state*>(opaque));
}

void seal_primitive(void*, std::span<const std::byte, 12>, std::span<const std::byte>,
    std::span<const std::byte> plaintext, std::span<std::byte> output) {
    if (output.size() < plaintext.size()) {
        throw std::invalid_argument("output too small");
    }
    std::ranges::copy(plaintext, output.begin());
}

ruvia::quic_aead_key_operations::open_result open_primitive(
    void*, std::span<const std::byte, 12>, std::span<const std::byte>,
    std::span<const std::byte>, std::span<std::byte>) {
    return {.value_ = ruvia::quic_aead_key_operations::open_result::status::rejected};
}

void mask_primitive(void*, std::span<const std::byte, 16>, std::span<std::byte, 5> output) {
    std::ranges::fill(output, std::byte{0x5a});
}

ruvia::quic_aead_key create_aead(void* context_value, ruvia::quic_cipher_suite,
    ruvia::quic_crypto_direction, std::span<const std::byte>) {
    auto* counts = static_cast<allocation_counts*>(context_value);
    auto state_value = std::make_unique<primitive_state>(counts);
    ++counts->allocated_;
    auto key = ruvia::quic_aead_key::adopt(
        state_value.get(), {.destroy_ = destroy_primitive, .seal_ = seal_primitive, .open_ = open_primitive});
    state_value.release();
    return key;
}

ruvia::quic_header_protection_key create_header_key(void* context_value, ruvia::quic_cipher_suite,
    std::span<const std::byte>) {
    auto* counts = static_cast<allocation_counts*>(context_value);
    auto state_value = std::make_unique<primitive_state>(counts);
    ++counts->allocated_;
    auto key = ruvia::quic_header_protection_key::adopt(
        state_value.get(), {.destroy_ = destroy_primitive, .mask_ = mask_primitive});
    state_value.release();
    return key;
}

void no_erasure_is_needed_for_test_state(void*, std::span<std::byte>) noexcept {}
void fill_random(void*, std::span<std::byte> output) {
    std::ranges::fill(output, std::byte{0x7f});
}
void hkdf_extract(void*, ruvia::quic_cipher_suite, std::span<const std::byte>,
    std::span<const std::byte>, std::span<std::byte>) {}
void hkdf_expand(void*, ruvia::quic_cipher_suite, std::span<const std::byte>,
    std::span<const std::byte>, std::span<std::byte>) {}

}  // namespace

RUVIA_TEST(quic_crypto_key_ownership_covers_move_replacement_rejection_and_failed_adoption) {
    allocation_counts counts;
    const ruvia::quic_crypto_provider_view provider{
        .context_ = &counts,
        .random_bytes_ = fill_random,
        .hkdf_extract_ = hkdf_extract,
        .hkdf_expand_ = hkdf_expand,
        .create_aead_key_ = create_aead,
        .create_header_protection_key_ = create_header_key,
        .secure_erase_ = no_erasure_is_needed_for_test_state,
    };
    provider.validate();
    std::array<std::byte, 16> key_material{};
    {
        auto old_key = provider.create_aead_key_(provider.context_,
            ruvia::quic_cipher_suite::aes_128_gcm_sha256, ruvia::quic_crypto_direction::write,
            key_material);
        auto replacement = provider.create_aead_key_(provider.context_,
            ruvia::quic_cipher_suite::aes_256_gcm_sha384, ruvia::quic_crypto_direction::read,
            key_material);
        RUVIA_CHECK_EQ(counts.allocated_, std::size_t{2});
        replacement = std::move(old_key);
        RUVIA_CHECK(!old_key);
        RUVIA_CHECK_EQ(counts.released_, std::size_t{1});

        std::array<std::byte, 12> nonce{};
        std::array<std::byte, 4> plaintext{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
        std::array<std::byte, 4> ciphertext{};
        replacement.seal(nonce, {}, plaintext, ciphertext);
        RUVIA_CHECK_EQ(ciphertext, plaintext);
        const auto rejected = replacement.open(nonce, {}, ciphertext, plaintext);
        RUVIA_CHECK(rejected.value_ == ruvia::quic_aead_key_operations::open_result::status::rejected);
        RUVIA_CHECK_EQ(rejected.plaintext_size_, std::size_t{0});
    }
    RUVIA_CHECK_EQ(counts.released_, std::size_t{2});

    auto unstarted_owner = std::make_unique<primitive_state>(&counts);
    ++counts.allocated_;
    try {
        (void)ruvia::quic_aead_key::adopt(unstarted_owner.get(),
            {.destroy_ = destroy_primitive, .seal_ = seal_primitive, .open_ = nullptr});
        RUVIA_CHECK(false);
    } catch (const std::invalid_argument&) {
        RUVIA_CHECK_EQ(counts.released_, std::size_t{2});
    }
    unstarted_owner.reset();
    RUVIA_CHECK_EQ(counts.allocated_, counts.released_);
}

RUVIA_TEST(quic_header_protection_key_move_and_destruction_return_owned_state) {
    allocation_counts counts;
    auto state_value = std::make_unique<primitive_state>(&counts);
    ++counts.allocated_;
    auto key = ruvia::quic_header_protection_key::adopt(
        state_value.get(), {.destroy_ = destroy_primitive, .mask_ = mask_primitive});
    state_value.release();
    auto moved = std::move(key);
    RUVIA_CHECK(!key);
    RUVIA_CHECK(moved);
    std::array<std::byte, 16> sample{};
    std::array<std::byte, 5> mask{};
    moved.mask(sample, mask);
    RUVIA_CHECK(std::ranges::all_of(mask, [](std::byte value) { return value == std::byte{0x5a}; }));
    moved = {};
    RUVIA_CHECK_EQ(counts.allocated_, counts.released_);
}
