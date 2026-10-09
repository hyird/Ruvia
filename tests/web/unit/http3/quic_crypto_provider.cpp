#include <algorithm>
#include <array>
#include <cstddef>
#include <memory_resource>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "http3/openssl_quic_crypto_provider.h"
#include "test_harness.h"

namespace {

struct counting_resource final : std::pmr::memory_resource {
    std::size_t allocations{};
    std::size_t deallocations{};
    std::size_t live_bytes{};

    void* do_allocate(std::size_t size, std::size_t alignment) override {
        void* const pointer = std::pmr::new_delete_resource()->allocate(size, alignment);
        ++allocations;
        live_bytes += size;
        return pointer;
    }
    void do_deallocate(void* pointer, std::size_t size, std::size_t alignment) override {
        ++deallocations;
        live_bytes -= size;
        std::pmr::new_delete_resource()->deallocate(pointer, size, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

unsigned hex_digit(char value) {
    if (value >= '0' && value <= '9') {
        return static_cast<unsigned>(value - '0');
    }
    if (value >= 'a' && value <= 'f') {
        return static_cast<unsigned>(value - 'a' + 10);
    }
    if (value >= 'A' && value <= 'F') {
        return static_cast<unsigned>(value - 'A' + 10);
    }
    throw std::invalid_argument("invalid hexadecimal test vector");
}

std::vector<std::byte> hex_bytes(std::string_view value) {
    if (value.size() % 2 != 0) {
        throw std::invalid_argument("odd hexadecimal test vector length");
    }
    std::vector<std::byte> bytes;
    bytes.reserve(value.size() / 2);
    for (std::size_t index = 0; index < value.size(); index += 2) {
        bytes.push_back(static_cast<std::byte>((hex_digit(value[index]) << 4) | hex_digit(value[index + 1])));
    }
    return bytes;
}

std::vector<std::byte> tls13_label(std::string_view label, std::size_t output_size) {
    constexpr std::string_view prefix = "tls13 ";
    std::vector<std::byte> info;
    info.reserve(4 + prefix.size() + label.size());
    info.push_back(static_cast<std::byte>((output_size >> 8) & 0xff));
    info.push_back(static_cast<std::byte>(output_size & 0xff));
    info.push_back(static_cast<std::byte>(prefix.size() + label.size()));
    for (const char value : prefix) {
        info.push_back(static_cast<std::byte>(value));
    }
    for (const char value : label) {
        info.push_back(static_cast<std::byte>(value));
    }
    info.push_back(std::byte{0});
    return info;
}

}  // namespace

RUVIA_TEST(openssl_quic_crypto_provider_supports_all_tls13_suites_and_authentication) {
    ruvia::detail::openssl_quic_crypto_provider provider(std::pmr::get_default_resource());
    const auto crypto = provider.view();
    crypto.validate();
    for (const auto suite : {ruvia::quic_cipher_suite::aes_128_gcm_sha256,
             ruvia::quic_cipher_suite::aes_256_gcm_sha384,
             ruvia::quic_cipher_suite::chacha20_poly1305_sha256}) {
        const std::size_t key_size = suite == ruvia::quic_cipher_suite::aes_128_gcm_sha256 ? 16 : 32;
        std::array<std::byte, 32> key_material{};
        std::array<std::byte, 12> nonce{};
        std::array<std::byte, 19> plaintext{};
        std::array<std::byte, 7> associated_data{};
        std::array<std::byte, 35> encrypted{};
        std::array<std::byte, 19> decrypted{};
        std::array<std::byte, 5> mask{};
        std::array<std::byte, 16> sample{};
        for (std::size_t i = 0; i < key_material.size(); ++i) {
            key_material[i] = std::byte(i + 1);
        }
        for (std::size_t i = 0; i < plaintext.size(); ++i) {
            plaintext[i] = std::byte(i * 3);
        }
        associated_data = {std::byte{0x51}, std::byte{0x55}, std::byte{0x49}, std::byte{0x43},
            std::byte{0x2d}, std::byte{0x41}, std::byte{0x44}};
        auto aead = crypto.create_aead_key(crypto.context, suite, ruvia::quic_crypto_direction::write,
            std::span(key_material).first(key_size));
        aead.seal(nonce, associated_data, plaintext, encrypted);
        const auto opened = aead.open(nonce, associated_data, encrypted, decrypted);
        RUVIA_CHECK(opened.value == ruvia::quic_aead_key_operations::open_result::status::authenticated);
        RUVIA_CHECK(opened.plaintext_size == plaintext.size());
        RUVIA_CHECK(decrypted == plaintext);
        encrypted.back() ^= std::byte{1};
        const auto rejected_tag = aead.open(nonce, associated_data, encrypted, decrypted);
        RUVIA_CHECK(rejected_tag.value == ruvia::quic_aead_key_operations::open_result::status::rejected);
        encrypted.back() ^= std::byte{1};
        associated_data.front() ^= std::byte{1};
        const auto rejected_aad = aead.open(nonce, associated_data, encrypted, decrypted);
        RUVIA_CHECK(rejected_aad.value == ruvia::quic_aead_key_operations::open_result::status::rejected);

        auto hp = crypto.create_header_protection_key(crypto.context, suite,
            std::span(key_material).first(key_size));
        hp.mask(sample, mask);
        RUVIA_CHECK((mask != std::array<std::byte, 5>{}));
    }
}

RUVIA_TEST(openssl_quic_crypto_provider_matches_rfc5869_sha256_vector) {
    ruvia::detail::openssl_quic_crypto_provider provider(std::pmr::get_default_resource());
    const auto crypto = provider.view();
    std::array<std::byte, 22> ikm{};
    ikm.fill(std::byte{0x0b});
    constexpr std::array<std::byte, 13> salt{
        std::byte{0x00}, std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04},
        std::byte{0x05}, std::byte{0x06}, std::byte{0x07}, std::byte{0x08}, std::byte{0x09},
        std::byte{0x0a}, std::byte{0x0b}, std::byte{0x0c}};
    constexpr std::array<std::byte, 10> info{
        std::byte{0xf0}, std::byte{0xf1}, std::byte{0xf2}, std::byte{0xf3}, std::byte{0xf4},
        std::byte{0xf5}, std::byte{0xf6}, std::byte{0xf7}, std::byte{0xf8}, std::byte{0xf9}};
    constexpr std::array<std::byte, 32> expected_prk{
        std::byte{0x07}, std::byte{0x77}, std::byte{0x09}, std::byte{0x36}, std::byte{0x2c}, std::byte{0x2e},
        std::byte{0x32}, std::byte{0xdf}, std::byte{0x0d}, std::byte{0xdc}, std::byte{0x3f}, std::byte{0x0d},
        std::byte{0xc4}, std::byte{0x7b}, std::byte{0xba}, std::byte{0x63}, std::byte{0x90}, std::byte{0xb6},
        std::byte{0xc7}, std::byte{0x3b}, std::byte{0xb5}, std::byte{0x0f}, std::byte{0x9c}, std::byte{0x31},
        std::byte{0x22}, std::byte{0xec}, std::byte{0x84}, std::byte{0x4a}, std::byte{0xd7}, std::byte{0xc2},
        std::byte{0xb3}, std::byte{0xe5}};
    std::array<std::byte, 32> prk{};
    crypto.hkdf_extract(crypto.context, ruvia::quic_cipher_suite::aes_128_gcm_sha256, salt, ikm, prk);
    RUVIA_CHECK(prk == expected_prk);
    std::array<std::byte, 42> okm{};
    crypto.hkdf_expand(crypto.context, ruvia::quic_cipher_suite::aes_128_gcm_sha256, prk, info, okm);
    RUVIA_CHECK(std::vector<std::byte>(okm.begin(), okm.end()) ==
                hex_bytes("3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865"));

    // RFC 5869 A.3 includes zero-length salt and info.
    crypto.hkdf_extract(crypto.context, ruvia::quic_cipher_suite::aes_128_gcm_sha256, {}, ikm, prk);
    RUVIA_CHECK(std::vector<std::byte>(prk.begin(), prk.end()) ==
                hex_bytes("19ef24a32c717b167f33a91d6f648bdf96596776afdb6377ac434c1c293ccb04"));
    crypto.hkdf_expand(crypto.context, ruvia::quic_cipher_suite::aes_128_gcm_sha256, prk, {}, okm);
    RUVIA_CHECK(std::vector<std::byte>(okm.begin(), okm.end()) ==
                hex_bytes("8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d9d201395faa4b61a96c8"));
}

RUVIA_TEST(openssl_quic_crypto_provider_rejects_invalid_provider_inputs) {
    ruvia::detail::openssl_quic_crypto_provider provider(std::pmr::get_default_resource());
    const auto crypto = provider.view();
    RUVIA_CHECK(ruvia::testing::throwsOn([&] {
        crypto.create_aead_key(crypto.context, ruvia::quic_cipher_suite::aes_128_gcm_sha256,
            ruvia::quic_crypto_direction::read, std::array<std::byte, 15>{});
    }));
    RUVIA_CHECK(ruvia::testing::throwsOn([&] { ruvia::quic_crypto_provider_view{}.validate(); }));
}

RUVIA_TEST(openssl_quic_crypto_provider_matches_rfc9001_initial_key_vectors) {
    using namespace ruvia;
    detail::openssl_quic_crypto_provider provider(std::pmr::get_default_resource());
    const auto crypto = provider.view();
    const auto salt = hex_bytes("38762cf7f55934b34d179ae6a4c80cadccbb7f0a");
    const auto destination_connection_id = hex_bytes("8394c8f03e515708");
    std::array<std::byte, 32> initial_secret{};
    crypto.hkdf_extract(crypto.context, quic_cipher_suite::aes_128_gcm_sha256,
        salt, destination_connection_id, initial_secret);
    RUVIA_CHECK(std::vector<std::byte>(initial_secret.begin(), initial_secret.end()) ==
                hex_bytes("7db5df06e7a69e432496adedb00851923595221596ae2ae9fb8115c1e9ed0a44"));

    std::array<std::byte, 32> client_secret{};
    std::array<std::byte, 32> server_secret{};
    const auto client_label = tls13_label("client in", client_secret.size());
    const auto server_label = tls13_label("server in", server_secret.size());
    crypto.hkdf_expand(crypto.context, quic_cipher_suite::aes_128_gcm_sha256,
        initial_secret, client_label, client_secret);
    crypto.hkdf_expand(crypto.context, quic_cipher_suite::aes_128_gcm_sha256,
        initial_secret, server_label, server_secret);
    RUVIA_CHECK(std::vector<std::byte>(client_secret.begin(), client_secret.end()) ==
                hex_bytes("c00cf151ca5be075ed0ebfb5c80323c42d6b7db67881289af4008f1f6c357aea"));
    RUVIA_CHECK(std::vector<std::byte>(server_secret.begin(), server_secret.end()) ==
                hex_bytes("3c199828fd139efd216c155ad844cc81fb82fa8d7446fa7d78be803acdda951b"));

    const auto check_keys = [&](std::span<const std::byte> secret, std::string_view expected_key,
                                std::string_view expected_iv, std::string_view expected_hp) {
        std::array<std::byte, 16> key{};
        std::array<std::byte, 12> iv{};
        std::array<std::byte, 16> hp_key{};
        const auto key_label = tls13_label("quic key", key.size());
        const auto iv_label = tls13_label("quic iv", iv.size());
        const auto hp_label = tls13_label("quic hp", hp_key.size());
        crypto.hkdf_expand(crypto.context, quic_cipher_suite::aes_128_gcm_sha256, secret, key_label, key);
        crypto.hkdf_expand(crypto.context, quic_cipher_suite::aes_128_gcm_sha256, secret, iv_label, iv);
        crypto.hkdf_expand(crypto.context, quic_cipher_suite::aes_128_gcm_sha256, secret, hp_label, hp_key);
        RUVIA_CHECK(std::vector<std::byte>(key.begin(), key.end()) == hex_bytes(expected_key));
        RUVIA_CHECK(std::vector<std::byte>(iv.begin(), iv.end()) == hex_bytes(expected_iv));
        RUVIA_CHECK(std::vector<std::byte>(hp_key.begin(), hp_key.end()) == hex_bytes(expected_hp));
    };
    check_keys(client_secret, "1f369613dd76d5467730efcbe3b1a22d",
        "fa044b2f42a3fd3b46fb255c", "9f50449e04a0e810283a1e9933adedd2");
    check_keys(server_secret, "cf3a5331653c364c88f0f379b6067e37",
        "0ac1493ca1905853b0bba03e", "c206b8d9b9f0f37644430b490eeaa314");

    auto header_key = crypto.create_header_protection_key(crypto.context,
        quic_cipher_suite::aes_128_gcm_sha256,
        hex_bytes("9f50449e04a0e810283a1e9933adedd2"));
    const auto sample_bytes = hex_bytes("d1b1c98dd7689fb8ec11d242b123dc9b");
    std::array<std::byte, 5> mask{};
    header_key.mask(std::span<const std::byte, 16>(sample_bytes.data(), 16), mask);
    RUVIA_CHECK(std::vector<std::byte>(mask.begin(), mask.end()) == hex_bytes("437b9aec36"));
}

RUVIA_TEST(openssl_quic_crypto_provider_decrypts_rfc9001_client_initial_packet_vector) {
    using namespace ruvia;
    detail::openssl_quic_crypto_provider provider(std::pmr::get_default_resource());
    const auto crypto = provider.view();
    const auto packet = hex_bytes(
        "c000000001088394c8f03e5157080000449e7b9aec34d1b1c98dd7689fb8ec11"
        "d242b123dc9bd8bab936b47d92ec356c0bab7df5976d27cd449f63300099f399"
        "1c260ec4c60d17b31f8429157bb35a1282a643a8d2262cad67500cadb8e7378c"
        "8eb7539ec4d4905fed1bee1fc8aafba17c750e2c7ace01e6005f80fcb7df6212"
        "30c83711b39343fa028cea7f7fb5ff89eac2308249a02252155e2347b63d58c5"
        "457afd84d05dfffdb20392844ae812154682e9cf012f9021a6f0be17ddd0c208"
        "4dce25ff9b06cde535d0f920a2db1bf362c23e596d11a4f5a6cf3948838a3aec"
        "4e15daf8500a6ef69ec4e3feb6b1d98e610ac8b7ec3faf6ad760b7bad1db4ba3"
        "485e8a94dc250ae3fdb41ed15fb6a8e5eba0fc3dd60bc8e30c5c4287e53805db"
        "059ae0648db2f64264ed5e39be2e20d82df566da8dd5998ccabdae053060ae6c"
        "7b4378e846d29f37ed7b4ea9ec5d82e7961b7f25a9323851f681d582363aa5f8"
        "9937f5a67258bf63ad6f1a0b1d96dbd4faddfcefc5266ba6611722395c906556"
        "be52afe3f565636ad1b17d508b73d8743eeb524be22b3dcbc2c7468d54119c74"
        "68449a13d8e3b95811a198f3491de3e7fe942b330407abf82a4ed7c1b311663a"
        "c69890f4157015853d91e923037c227a33cdd5ec281ca3f79c44546b9d90ca00"
        "f064c99e3dd97911d39fe9c5d0b23a229a234cb36186c4819e8b9c5927726632"
        "291d6a418211cc2962e20fe47feb3edf330f2c603a9d48c0fcb5699dbfe58964"
        "25c5bac4aee82e57a85aaf4e2513e4f05796b07ba2ee47d80506f8d2c25e50fd"
        "14de71e6c418559302f939b0e1abd576f279c4b2e0feb85c1f28ff18f58891ff"
        "ef132eef2fa09346aee33c28eb130ff28f5b766953334113211996d20011a198"
        "e3fc433f9f2541010ae17c1bf202580f6047472fb36857fe843b19f5984009dd"
        "c324044e847a4f4a0ab34f719595de37252d6235365e9b84392b061085349d73"
        "203a4a13e96f5432ec0fd4a1ee65accdd5e3904df54c1da510b0ff20dcc0c77f"
        "cb2c0e0eb605cb0504db87632cf3d8b4dae6e705769d1de354270123cb11450e"
        "fc60ac47683d7b8d0f811365565fd98c4c8eb936bcab8d069fc33bd801b03ade"
        "a2e1fbc5aa463d08ca19896d2bf59a071b851e6c239052172f296bfb5e724047"
        "90a2181014f3b94a4e97d117b438130368cc39dbb2d198065ae3986547926cd2"
        "162f40a29f0c3c8745c0f50fba3852e566d44575c29d39a03f0cda721984b6f4"
        "40591f355e12d439ff150aab7613499dbd49adabc8676eef023b15b65bfc5ca0"
        "6948109f23f350db82123535eb8a7433bdabcb909271a6ecbcb58b936a88cd4e"
        "8f2e6ff5800175f113253d8fa9ca8885c2f552e657dc603f252e1a8e308f76f0"
        "be79e2fb8f5d5fbbe2e30ecadd220723c8c0aea8078cdfcb3868263ff8f09400"
        "54da48781893a7e49ad5aff4af300cd804a6b6279ab3ff3afb64491c85194aab"
        "760d58a606654f9f4400e8b38591356fbf6425aca26dc85244259ff2b19c41b9"
        "f96f3ca9ec1dde434da7d2d392b905ddf3d1f9af93d1af5950bd493f5aa731b4"
        "056df31bd267b6b90a079831aaf579be0a39013137aac6d404f518cfd4684064"
        "7e78bfe706ca4cf5e9c5453e9f7cfd2b8b4c8d169a44e55c88d4a9a7f9474241"
        "e221af44860018ab0856972e194cd934");
    RUVIA_CHECK(packet.size() == 1200);

    auto header_key = crypto.create_header_protection_key(crypto.context,
        quic_cipher_suite::aes_128_gcm_sha256,
        hex_bytes("9f50449e04a0e810283a1e9933adedd2"));
    constexpr std::size_t packet_number_offset = 18;
    constexpr std::size_t header_size = packet_number_offset + 4;
    constexpr std::array<std::byte, 16> rfc_sample{
        std::byte{0xd1}, std::byte{0xb1}, std::byte{0xc9}, std::byte{0x8d}, std::byte{0xd7}, std::byte{0x68},
        std::byte{0x9f}, std::byte{0xb8}, std::byte{0xec}, std::byte{0x11}, std::byte{0xd2}, std::byte{0x42},
        std::byte{0xb1}, std::byte{0x23}, std::byte{0xdc}, std::byte{0x9b}};
    const std::span<const std::byte, 16> sample(packet.data() + packet_number_offset + 4, 16);
    RUVIA_CHECK(std::equal(sample.begin(), sample.end(), rfc_sample.begin()));
    std::array<std::byte, 5> mask{};
    header_key.mask(sample, mask);
    RUVIA_CHECK(std::vector<std::byte>(mask.begin(), mask.end()) == hex_bytes("437b9aec36"));
    auto unprotected_header = std::vector<std::byte>(packet.begin(), packet.begin() + header_size);
    unprotected_header[0] ^= mask[0] & std::byte{0x0f};
    for (std::size_t index = 0; index < 4; ++index) {
        unprotected_header[packet_number_offset + index] ^= mask[index + 1];
    }
    RUVIA_CHECK(unprotected_header == hex_bytes("c300000001088394c8f03e5157080000449e00000002"));
    std::uint64_t packet_number{};
    for (std::size_t index = packet_number_offset; index < header_size; ++index) {
        packet_number = (packet_number << 8) | std::to_integer<std::uint8_t>(unprotected_header[index]);
    }
    RUVIA_CHECK(packet_number == 2);

    auto iv = hex_bytes("fa044b2f42a3fd3b46fb255c");
    for (std::size_t index = 0; index < sizeof(packet_number); ++index) {
        iv[iv.size() - index - 1] ^= static_cast<std::byte>((packet_number >> (index * 8)) & 0xff);
    }
    RUVIA_CHECK(iv == hex_bytes("fa044b2f42a3fd3b46fb255e"));
    auto aead = crypto.create_aead_key(crypto.context,
        quic_cipher_suite::aes_128_gcm_sha256, quic_crypto_direction::read,
        hex_bytes("1f369613dd76d5467730efcbe3b1a22d"));
    std::vector<std::byte> plaintext(1162);
    const auto opened = aead.open(std::span<const std::byte, 12>(iv.data(), 12),
        unprotected_header, std::span<const std::byte>(packet).subspan(header_size), plaintext);
    RUVIA_CHECK(opened.value == quic_aead_key_operations::open_result::status::authenticated);
    RUVIA_CHECK(opened.plaintext_size == plaintext.size());
    const auto rfc_crypto_frame = hex_bytes(
        "060040f1010000ed0303ebf8fa56f12939b9584a3896472ec40bb863cfd3e868"
        "04fe3a47f06a2b69484c00000413011302010000c000000010000e00000b6578"
        "616d706c652e636f6dff01000100000a00080006001d00170018001000070005"
        "04616c706e000500050100000000003300260024001d00209370b2c9caa47fbabaf4"
        "559fedba753de171fa71f50f1ce15d43e994ec74d748002b000302030400"
        "0d0010000e0403050306030203080408050806002d00020101001c0002400100"
        "3900320408ffffffffffffffff05048000ffff07048000ffff08011001048000"
        "75300901100f088394c8f03e51570806048000ffff");
    RUVIA_CHECK(rfc_crypto_frame.size() == 245);
    RUVIA_CHECK(std::equal(rfc_crypto_frame.begin(), rfc_crypto_frame.end(), plaintext.begin()));
    RUVIA_CHECK(std::all_of(plaintext.begin() + 245, plaintext.end(),
        [](std::byte value) { return value == std::byte{0}; }));
}

RUVIA_TEST(openssl_quic_crypto_provider_handles_empty_payload_and_nonempty_aad) {
    counting_resource resource;
    {
        ruvia::detail::openssl_quic_crypto_provider provider(&resource);
        const auto crypto = provider.view();
        const auto provider_live_bytes = resource.live_bytes;
        const std::array<std::byte, 29> aad{
            std::byte{8}, std::byte{0x83}, std::byte{0x94}, std::byte{0xc8}, std::byte{0xf0},
            std::byte{0x3e}, std::byte{0x51}, std::byte{0x57}, std::byte{0x08}, std::byte{0xff},
            std::byte{0}, std::byte{0}, std::byte{0}, std::byte{1}, std::byte{0}, std::byte{8},
            std::byte{0xf0}, std::byte{0x67}, std::byte{0xa5}, std::byte{0x50}, std::byte{0x2a},
            std::byte{0x42}, std::byte{0x62}, std::byte{0xb5}, std::byte{'t'}, std::byte{'o'},
            std::byte{'k'}, std::byte{'e'}, std::byte{'n'}};
        const std::array<std::byte, 12> nonce{
            std::byte{0x46}, std::byte{0x15}, std::byte{0x99}, std::byte{0xd3}, std::byte{0x5d},
            std::byte{0x63}, std::byte{0x2b}, std::byte{0xf2}, std::byte{0x23}, std::byte{0x98},
            std::byte{0x25}, std::byte{0xbb}};
        for (const auto suite : {ruvia::quic_cipher_suite::aes_128_gcm_sha256,
                 ruvia::quic_cipher_suite::aes_256_gcm_sha384,
                 ruvia::quic_cipher_suite::chacha20_poly1305_sha256}) {
            const std::size_t key_size = suite == ruvia::quic_cipher_suite::aes_128_gcm_sha256 ? 16 : 32;
            std::array<std::byte, 32> key_material{};
            key_material.fill(std::byte{0x5a});
            {
                auto aead = crypto.create_aead_key(crypto.context, suite,
                    ruvia::quic_crypto_direction::write,
                    std::span<const std::byte>(key_material).first(key_size));
                std::array<std::byte, 16> tag{};
                aead.seal(nonce, aad, std::span<const std::byte>{}, tag);
                std::span<std::byte> empty_output;
                const auto opened = aead.open(nonce, aad, tag, empty_output);
                RUVIA_CHECK(opened.value == ruvia::quic_aead_key_operations::open_result::status::authenticated);
                RUVIA_CHECK(opened.plaintext_size == 0);

                tag.back() ^= std::byte{1};
                const auto rejected = aead.open(nonce, aad, tag, empty_output);
                RUVIA_CHECK(rejected.value == ruvia::quic_aead_key_operations::open_result::status::rejected);
                RUVIA_CHECK(rejected.plaintext_size == 0);

                std::array<std::byte, 15> short_tag{};
                RUVIA_CHECK(ruvia::testing::throwsOn([&] {
                    aead.seal(nonce, aad, std::span<const std::byte>{}, short_tag);
                }));
                const std::array<std::byte, 3> plaintext{
                    std::byte{0x31}, std::byte{0x32}, std::byte{0x33}};
                std::array<std::byte, 19> ciphertext{};
                aead.seal(nonce, aad, plaintext, ciphertext);
                std::array<std::byte, 2> short_plaintext{};
                RUVIA_CHECK(ruvia::testing::throwsOn([&] {
                    (void)aead.open(nonce, aad, ciphertext, short_plaintext);
                }));
                std::array<std::byte, 3> rejected_plaintext{};
                ciphertext.back() ^= std::byte{1};
                const auto rejected_payload = aead.open(nonce, aad, ciphertext, rejected_plaintext);
                RUVIA_CHECK(rejected_payload.value == ruvia::quic_aead_key_operations::open_result::status::rejected);
                RUVIA_CHECK(std::all_of(rejected_plaintext.begin(), rejected_plaintext.end(),
                    [](std::byte value) { return value == std::byte{0}; }));
            }
            crypto.secure_erase(crypto.context, key_material);
            RUVIA_CHECK(resource.live_bytes == provider_live_bytes);
        }
    }
    RUVIA_CHECK(resource.allocations == resource.deallocations);
    RUVIA_CHECK(resource.live_bytes == 0);
}

RUVIA_TEST(openssl_quic_crypto_provider_matches_rfc9001_retry_integrity_tag) {
    using namespace ruvia;
    detail::openssl_quic_crypto_provider provider(std::pmr::get_default_resource());
    const auto crypto = provider.view();
    const auto retry_secret = hex_bytes(
        "d9c9943e6101fd200021506bcc02814c73030f25c79d71ce876eca876e6fca8e");
    const auto key_label = tls13_label("quic key", 16);
    const auto iv_label = tls13_label("quic iv", 12);
    std::array<std::byte, 16> key{};
    std::array<std::byte, 12> nonce{};
    crypto.hkdf_expand(crypto.context, quic_cipher_suite::aes_128_gcm_sha256,
        retry_secret, key_label, key);
    crypto.hkdf_expand(crypto.context, quic_cipher_suite::aes_128_gcm_sha256,
        retry_secret, iv_label, nonce);
    RUVIA_CHECK(std::vector<std::byte>(key.begin(), key.end()) ==
                hex_bytes("be0c690b9f66575a1d766b54e368c84e"));
    RUVIA_CHECK(std::vector<std::byte>(nonce.begin(), nonce.end()) ==
                hex_bytes("461599d35d632bf2239825bb"));

    const auto pseudo_packet = hex_bytes(
        "088394c8f03e515708ff000000010008f067a5502a4262b5746f6b656e");
    auto retry_key = crypto.create_aead_key(crypto.context,
        quic_cipher_suite::aes_128_gcm_sha256, quic_crypto_direction::write, key);
    std::array<std::byte, 16> tag{};
    retry_key.seal(nonce, pseudo_packet, {}, tag);
    RUVIA_CHECK(std::vector<std::byte>(tag.begin(), tag.end()) ==
                hex_bytes("04a265ba2eff4d829058fb3f0f2496ba"));
    tag.back() ^= std::byte{1};
    std::array<std::byte, 1> plaintext{};
    const auto rejected = retry_key.open(nonce, pseudo_packet, tag, plaintext);
    RUVIA_CHECK(rejected.value == quic_aead_key_operations::open_result::status::rejected);
}
