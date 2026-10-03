#include "ruvia/http/detail/http3/quic_key_schedule.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "test_harness.h"

namespace {

using bytes = std::vector<std::byte>;

bytes as_bytes(std::string_view value) {
    bytes result;
    for (const auto character : value) {
        result.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
    }
    return result;
}

bytes hkdf_info(std::size_t output_size, std::string_view label,
    std::span<const std::byte> context = {}) {
    const auto full_label = as_bytes(std::string("tls13 ") + std::string(label));
    bytes info{static_cast<std::byte>((output_size >> 8) & 0xff),
        static_cast<std::byte>(output_size & 0xff),
        static_cast<std::byte>(full_label.size())};
    info.insert(info.end(), full_label.begin(), full_label.end());
    info.push_back(static_cast<std::byte>(context.size()));
    info.insert(info.end(), context.begin(), context.end());
    return info;
}

struct counting_resource final : std::pmr::memory_resource {
    std::size_t allocations{};
    std::size_t deallocations{};

private:
    void* do_allocate(std::size_t size, std::size_t alignment) override {
        ++allocations;
        return std::pmr::new_delete_resource()->allocate(size, alignment);
    }
    void do_deallocate(void* pointer, std::size_t size, std::size_t alignment) override {
        ++deallocations;
        std::pmr::new_delete_resource()->deallocate(pointer, size, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

struct provider_state {
    bytes extracted_salt;
    bytes extracted_input;
    std::vector<bytes> expand_infos;
    std::vector<bytes> factory_keys;
    bytes last_nonce;
    bytes last_aad;
    std::size_t erase_calls{};
    std::size_t erased_bytes{};
    std::size_t destroyed_keys{};
    bool fail_header_factory{};
};

void secure_erase(void* opaque, std::span<std::byte> value) noexcept {
    auto& state = *static_cast<provider_state*>(opaque);
    ++state.erase_calls;
    state.erased_bytes += value.size();
    std::ranges::fill(value, std::byte{});
}

void hkdf_extract(void* opaque, ruvia::quic_cipher_suite,
    std::span<const std::byte> salt, std::span<const std::byte> input,
    std::span<std::byte> output) {
    auto& state = *static_cast<provider_state*>(opaque);
    state.extracted_salt.assign(salt.begin(), salt.end());
    state.extracted_input.assign(input.begin(), input.end());
    std::ranges::fill(output, std::byte{0x11});
}

void hkdf_expand(void* opaque, ruvia::quic_cipher_suite,
    std::span<const std::byte> secret, std::span<const std::byte> info,
    std::span<std::byte> output) {
    auto& state = *static_cast<provider_state*>(opaque);
    if (secret.empty()) {
        throw std::invalid_argument("empty HKDF secret");
    }
    state.expand_infos.emplace_back(info.begin(), info.end());
    std::byte fill{};
    const auto label_size = info.size() >= 3 ? std::to_integer<std::size_t>(info[2]) : 0;
    if (info.size() >= 3 && label_size <= info.size() - 3) {
        constexpr auto prefix = std::array{std::byte{'t'}, std::byte{'l'}, std::byte{'s'},
            std::byte{'1'}, std::byte{'3'}, std::byte{' '}};
        const auto label = info.subspan(3, label_size);
        if (label.size() >= prefix.size() && std::ranges::equal(label.first(prefix.size()), prefix)) {
            const auto name = label.subspan(prefix.size());
            if (std::ranges::equal(name, as_bytes("client in"))) {
                fill = std::byte{0x21};
            } else if (std::ranges::equal(name, as_bytes("server in"))) {
                fill = std::byte{0x22};
            } else if (std::ranges::equal(name, as_bytes("quic key"))) {
                fill = std::byte{0xa1};
            } else if (std::ranges::equal(name, as_bytes("quic iv"))) {
                fill = std::byte{0xb2};
            } else if (std::ranges::equal(name, as_bytes("quic hp"))) {
                fill = std::byte{0xc3};
            } else if (std::ranges::equal(name, as_bytes("quic ku"))) {
                fill = std::byte{0xd4};
            }
        }
    }
    std::ranges::fill(output, fill);
}

struct key_state {
    provider_state* owner{};
    bytes key;
};

void destroy_key(void* opaque) noexcept {
    std::unique_ptr<key_state> state(static_cast<key_state*>(opaque));
    ++state->owner->destroyed_keys;
}

void seal(void* opaque, std::span<const std::byte, 12> nonce,
    std::span<const std::byte> aad, std::span<const std::byte> plaintext,
    std::span<std::byte> output) {
    auto& owner = *static_cast<key_state*>(opaque)->owner;
    owner.last_nonce.assign(nonce.begin(), nonce.end());
    owner.last_aad.assign(aad.begin(), aad.end());
    if (output.size() != plaintext.size() + 16) {
        throw std::invalid_argument("test AEAD expects a 16-byte tag");
    }
    std::ranges::fill(output, std::byte{0x5e});
}

ruvia::quic_aead_key create_aead(void* opaque, ruvia::quic_cipher_suite,
    ruvia::quic_crypto_direction, std::span<const std::byte> key) {
    auto& owner = *static_cast<provider_state*>(opaque);
    owner.factory_keys.emplace_back(key.begin(), key.end());
    auto state = std::make_unique<key_state>();
    state->owner = &owner;
    state->key.assign(key.begin(), key.end());
    return ruvia::quic_aead_key::adopt(state.release(),
        {.destroy = destroy_key, .seal = seal, .open = [](void*, std::span<const std::byte, 12>, std::span<const std::byte>, std::span<const std::byte>, std::span<std::byte>) {
             return ruvia::quic_aead_key_operations::open_result{};
         }});
}

ruvia::quic_header_protection_key create_header(void* opaque,
    ruvia::quic_cipher_suite, std::span<const std::byte> key) {
    auto& owner = *static_cast<provider_state*>(opaque);
    if (owner.fail_header_factory) {
        throw std::runtime_error("injected header-key allocation failure");
    }
    owner.factory_keys.emplace_back(key.begin(), key.end());
    auto state = std::make_unique<key_state>();
    state->owner = &owner;
    state->key.assign(key.begin(), key.end());
    return ruvia::quic_header_protection_key::adopt(state.release(),
        {.destroy = destroy_key,
            .mask = [](void*, std::span<const std::byte, 16>, std::span<std::byte, 5> output) {
                std::ranges::fill(output, std::byte{0x77});
            }});
}

ruvia::quic_crypto_provider_view make_provider(provider_state& state) {
    return {.context = &state,
        .random_bytes = [](void*, std::span<std::byte> output) {
            std::ranges::fill(output, std::byte{0x01});
        },
        .hkdf_extract = hkdf_extract,
        .hkdf_expand = hkdf_expand,
        .create_aead_key = create_aead,
        .create_header_protection_key = create_header,
        .secure_erase = secure_erase};
}

RUVIA_TEST(quic_cipher_suite_parameters_are_restricted_to_supported_v1_suites) {
    using ruvia::detail::quic_cipher_suite_parameters_for;
    const auto aes128 = quic_cipher_suite_parameters_for(ruvia::quic_cipher_suite::aes_128_gcm_sha256);
    RUVIA_CHECK_EQ(aes128.hash_size, std::size_t{32});
    RUVIA_CHECK_EQ(aes128.key_size, std::size_t{16});
    RUVIA_CHECK_EQ(aes128.iv_size, std::size_t{12});
    RUVIA_CHECK_EQ(aes128.tag_size, std::size_t{16});
    RUVIA_CHECK_EQ(aes128.max_encryptions, std::uint64_t{1} << 23);
    const auto aes256 = quic_cipher_suite_parameters_for(ruvia::quic_cipher_suite::aes_256_gcm_sha384);
    RUVIA_CHECK_EQ(aes256.hash_size, std::size_t{48});
    RUVIA_CHECK_EQ(aes256.key_size, std::size_t{32});
    const auto chacha = quic_cipher_suite_parameters_for(ruvia::quic_cipher_suite::chacha20_poly1305_sha256);
    RUVIA_CHECK_EQ(chacha.max_encryptions, std::uint64_t{1} << 62);
    RUVIA_CHECK(ruvia::testing::throwsOn([] {
        (void)quic_cipher_suite_parameters_for(static_cast<ruvia::quic_cipher_suite>(0x1304));
    }));
}

RUVIA_TEST(quic_tls13_hkdf_label_encoding_includes_prefix_lengths_label_and_context) {
    provider_state state;
    auto provider = make_provider(state);
    std::array<std::byte, 32> secret{};
    std::array<std::byte, 16> output{};
    const auto context = as_bytes("ctx");
    ruvia::detail::hkdf_expand_label(provider, ruvia::quic_cipher_suite::aes_128_gcm_sha256,
        secret, "quic key", context, output);
    const auto expected = hkdf_info(16, "quic key", context);
    RUVIA_CHECK_EQ(state.expand_infos.size(), std::size_t{1});
    RUVIA_CHECK_EQ(state.expand_infos.front(), expected);
    RUVIA_CHECK(std::ranges::all_of(output, [](std::byte value) { return value == std::byte{0xa1}; }));
    RUVIA_CHECK(ruvia::testing::throwsOn([&] {
        ruvia::detail::hkdf_expand_label(provider, ruvia::quic_cipher_suite::aes_128_gcm_sha256,
            std::span<const std::byte>(secret).first(16), "quic key", context, output);
    }));
}

RUVIA_TEST(quic_v1_initial_secrets_use_rfc_salt_and_client_server_labels) {
    provider_state state;
    counting_resource resource;
    auto provider = make_provider(state);
    const auto dcid = as_bytes("8394c8f03e515708");
    {
        auto secrets = ruvia::detail::derive_quic_initial_secrets(provider, &resource,
            ruvia::quic_version::v1, dcid);
        RUVIA_CHECK_EQ(secrets.client.view().size(), std::size_t{32});
        RUVIA_CHECK_EQ(secrets.server.view().size(), std::size_t{32});
        RUVIA_CHECK(std::ranges::all_of(secrets.client.view(), [](std::byte value) { return value == std::byte{0x21}; }));
        RUVIA_CHECK(std::ranges::all_of(secrets.server.view(), [](std::byte value) { return value == std::byte{0x22}; }));
        RUVIA_CHECK_EQ(state.extracted_input, dcid);
        const auto salt = as_bytes("\x38\x76\x2c\xf7\xf5\x59\x34\xb3\x4d\x17\x9a\xe6\xa4\xc8\x0c\xad\xcc\xbb\x7f\x0a");
        RUVIA_CHECK_EQ(state.extracted_salt, salt);
        RUVIA_CHECK_EQ(state.expand_infos.size(), std::size_t{2});
        RUVIA_CHECK_EQ(state.expand_infos[0], hkdf_info(32, "client in"));
        RUVIA_CHECK_EQ(state.expand_infos[1], hkdf_info(32, "server in"));
    }
    RUVIA_CHECK_EQ(resource.allocations, resource.deallocations);
    RUVIA_CHECK(state.erase_calls >= std::size_t{3});
}

RUVIA_TEST(quic_traffic_key_derivation_owns_material_and_erases_it_after_move) {
    provider_state state;
    counting_resource resource;
    auto provider = make_provider(state);
    std::array<std::byte, 32> traffic_secret{};
    {
        auto keys = ruvia::detail::derive_quic_packet_keys(provider, &resource,
            ruvia::quic_version::v1, ruvia::quic_cipher_suite::aes_128_gcm_sha256,
            ruvia::quic_crypto_direction::write, traffic_secret);
        auto moved = std::move(keys);
        RUVIA_CHECK_EQ(moved.traffic_secret().size(), std::size_t{32});
        RUVIA_CHECK_EQ(moved.iv().size(), std::size_t{12});
        RUVIA_CHECK_EQ(moved.max_encryptions(), std::uint64_t{1} << 23);
        RUVIA_CHECK_EQ(state.expand_infos.size(), std::size_t{3});
        RUVIA_CHECK_EQ(state.expand_infos[0], hkdf_info(16, "quic key"));
        RUVIA_CHECK_EQ(state.expand_infos[1], hkdf_info(12, "quic iv"));
        RUVIA_CHECK_EQ(state.expand_infos[2], hkdf_info(16, "quic hp"));
        RUVIA_CHECK_EQ(state.factory_keys.size(), std::size_t{2});
        RUVIA_CHECK(std::ranges::all_of(state.factory_keys[0], [](std::byte value) { return value == std::byte{0xa1}; }));
        RUVIA_CHECK(std::ranges::all_of(state.factory_keys[1], [](std::byte value) { return value == std::byte{0xc3}; }));
        RUVIA_CHECK(std::ranges::all_of(moved.iv(), [](std::byte value) { return value == std::byte{0xb2}; }));
    }
    RUVIA_CHECK_EQ(state.destroyed_keys, std::size_t{2});
    RUVIA_CHECK_EQ(resource.allocations, resource.deallocations);
    RUVIA_CHECK(state.erase_calls >= std::size_t{4});
    RUVIA_CHECK(state.erased_bytes >= std::size_t{32 + 16 + 12 + 16});
}

RUVIA_TEST(quic_key_update_derives_new_secret_without_replacing_header_protection) {
    provider_state state;
    counting_resource resource;
    auto provider = make_provider(state);
    std::array<std::byte, 32> traffic_secret{};
    {
        auto updated = ruvia::detail::update_quic_packet_keys(provider, &resource,
            ruvia::quic_version::v1, ruvia::quic_cipher_suite::aes_128_gcm_sha256,
            ruvia::quic_crypto_direction::read, traffic_secret);
        RUVIA_CHECK_EQ(updated.traffic_secret().size(), std::size_t{32});
        RUVIA_CHECK_EQ(updated.iv().size(), std::size_t{12});
        RUVIA_CHECK_EQ(state.expand_infos.size(), std::size_t{3});
        RUVIA_CHECK_EQ(state.expand_infos[0], hkdf_info(32, "quic ku"));
        RUVIA_CHECK_EQ(state.expand_infos[1], hkdf_info(16, "quic key"));
        RUVIA_CHECK_EQ(state.expand_infos[2], hkdf_info(12, "quic iv"));
        RUVIA_CHECK_EQ(state.factory_keys.size(), std::size_t{1});
    }
    RUVIA_CHECK_EQ(state.destroyed_keys, std::size_t{1});
    RUVIA_CHECK_EQ(resource.allocations, resource.deallocations);
}

RUVIA_TEST(quic_packet_key_factory_failure_releases_prior_key_and_secret_storage) {
    provider_state state;
    state.fail_header_factory = true;
    counting_resource resource;
    auto provider = make_provider(state);
    std::array<std::byte, 32> traffic_secret{};
    RUVIA_CHECK(ruvia::testing::throwsOn([&] {
        (void)ruvia::detail::derive_quic_packet_keys(provider, &resource,
            ruvia::quic_version::v1, ruvia::quic_cipher_suite::aes_128_gcm_sha256,
            ruvia::quic_crypto_direction::write, traffic_secret);
    }));
    RUVIA_CHECK_EQ(state.destroyed_keys, std::size_t{1});
    RUVIA_CHECK_EQ(resource.allocations, resource.deallocations);
    RUVIA_CHECK(state.erased_bytes >= std::size_t{32 + 16 + 12 + 16});
}

RUVIA_TEST(quic_v1_retry_integrity_uses_fixed_aes_key_nonce_and_pseudo_packet_aad) {
    provider_state state;
    auto provider = make_provider(state);
    const auto pseudo_packet = as_bytes("retry pseudo-packet");
    const auto tag = ruvia::detail::quic_retry_integrity_tag(
        provider, ruvia::quic_version::v1, pseudo_packet);
    // RFC 9001 Section 5.8 fixed bytes; this controlled provider checks parameter plumbing,
    // not the AES-GCM tag against an independent cryptographic vector.
    const auto expected_key = as_bytes("\xbe\x0c\x69\x0b\x9f\x66\x57\x5a\x1d\x76\x6b\x54\xe3\x68\xc8\x4e");
    const auto expected_nonce = as_bytes("\x46\x15\x99\xd3\x5d\x63\x2b\xf2\x23\x98\x25\xbb");
    RUVIA_CHECK_EQ(state.factory_keys.size(), std::size_t{1});
    RUVIA_CHECK_EQ(state.factory_keys.front(), expected_key);
    RUVIA_CHECK_EQ(state.last_nonce, expected_nonce);
    RUVIA_CHECK_EQ(state.last_aad, pseudo_packet);
    RUVIA_CHECK(std::ranges::all_of(tag, [](std::byte value) { return value == std::byte{0x5e}; }));
    RUVIA_CHECK_EQ(state.destroyed_keys, std::size_t{1});
}

RUVIA_TEST(quic_v2_initial_retry_and_key_update_use_rfc9369_parameters) {
    provider_state state;
    counting_resource resource;
    auto provider = make_provider(state);
    const auto dcid = as_bytes("8394c8f03e515708");
    {
        auto secrets = ruvia::detail::derive_quic_initial_secrets(provider, &resource,
            ruvia::quic_version::v2, dcid);
        const auto salt = as_bytes(std::string_view(
            "\x0d\xed\xe3\xde\xf7\x00\xa6\xdb\x81\x93\x81\xbe\x6e\x26\x9d\xcb\xf9\xbd\x2e\xd9", 20));
        RUVIA_CHECK_EQ(state.extracted_salt, salt);
    }
    state.expand_infos.clear();
    std::array<std::byte, 32> traffic_secret{};
    {
        auto keys = ruvia::detail::derive_quic_packet_keys(provider, &resource,
            ruvia::quic_version::v2, ruvia::quic_cipher_suite::aes_128_gcm_sha256,
            ruvia::quic_crypto_direction::write, traffic_secret);
        RUVIA_CHECK_EQ(state.expand_infos[0], hkdf_info(16, "quicv2 key"));
        RUVIA_CHECK_EQ(state.expand_infos[1], hkdf_info(12, "quicv2 iv"));
        RUVIA_CHECK_EQ(state.expand_infos[2], hkdf_info(16, "quicv2 hp"));
    }
    const auto pseudo_packet = as_bytes("retry pseudo-packet");
    (void)ruvia::detail::quic_retry_integrity_tag(provider, ruvia::quic_version::v2, pseudo_packet);
    const auto expected_key = as_bytes("\x8f\xb4\xb0\x1b\x56\xac\x48\xe2\x60\xfb\xcb\xce\xad\x7c\xcc\x92");
    const auto expected_nonce = as_bytes("\xd8\x69\x69\xbc\x2d\x7c\x6d\x99\x90\xef\xb0\x4a");
    RUVIA_CHECK_EQ(state.factory_keys.back(), expected_key);
    RUVIA_CHECK_EQ(state.last_nonce, expected_nonce);
    state.expand_infos.clear();
    {
        auto updated = ruvia::detail::update_quic_packet_keys(provider, &resource,
            ruvia::quic_version::v2, ruvia::quic_cipher_suite::aes_128_gcm_sha256,
            ruvia::quic_crypto_direction::read, traffic_secret);
        RUVIA_CHECK_EQ(state.expand_infos[0], hkdf_info(32, "quicv2 ku"));
    }
    RUVIA_CHECK_EQ(resource.allocations, resource.deallocations);
}

}  // namespace
