#include "ruvia/http/quic_server.h"

#include <ngtcp2/ngtcp2.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

#include "test_harness.h"

namespace {

void randomBytes(void* context, std::span<std::byte> output) {
    if (context) {
        throw std::runtime_error("controlled random provider failure");
    }
    static std::uint8_t next = 0x41;
    for (auto& byte : output) {
        byte = static_cast<std::byte>(next++);
    }
}

void hkdfExtract(void*, ruvia::quic_cipher_suite, std::span<const std::byte>,
    std::span<const std::byte>, std::span<std::byte>) {}
void hkdfExpand(void*, ruvia::quic_cipher_suite, std::span<const std::byte>,
    std::span<const std::byte>, std::span<std::byte>) {}
ruvia::quic_aead_key makeAead(void*, ruvia::quic_cipher_suite,
    ruvia::quic_crypto_direction, std::span<const std::byte>) {
    return {};
}
ruvia::quic_header_protection_key makeHeaderKey(void*, ruvia::quic_cipher_suite,
    std::span<const std::byte>) {
    return {};
}
void eraseBytes(void*, std::span<std::byte>) noexcept {}

ruvia::quic_crypto_provider_view provider() {
    return {.random_bytes = randomBytes,
        .hkdf_extract = hkdfExtract,
        .hkdf_expand = hkdfExpand,
        .create_aead_key = makeAead,
        .create_header_protection_key = makeHeaderKey,
        .secure_erase = eraseBytes};
}

struct deterministic_crypto final {
    std::uint8_t next{0x80};
    std::vector<std::vector<std::byte>> random_outputs;
    std::optional<std::vector<std::byte>> forced_random;
};

std::uint64_t hash_bytes(std::uint64_t hash, std::span<const std::byte> bytes) noexcept {
    for (const auto byte : bytes) {
        hash = (hash ^ std::to_integer<unsigned char>(byte)) * 1099511628211ULL;
    }
    return hash;
}

void fill_expansion(std::uint64_t seed, std::span<std::byte> output) noexcept {
    for (auto& byte : output) {
        seed ^= seed >> 12;
        seed ^= seed << 25;
        seed ^= seed >> 27;
        byte = static_cast<std::byte>((seed * 2685821657736338717ULL) >> 56);
    }
}

void deterministic_random(void* opaque, std::span<std::byte> output) {
    auto& crypto = *static_cast<deterministic_crypto*>(opaque);
    if (crypto.forced_random && crypto.forced_random->size() == output.size()) {
        std::ranges::copy(*crypto.forced_random, output.begin());
    } else {
        for (auto& byte : output) {
            byte = static_cast<std::byte>(crypto.next++);
        }
    }
    crypto.random_outputs.emplace_back(output.begin(), output.end());
}

void deterministic_extract(void*, ruvia::quic_cipher_suite,
    std::span<const std::byte> salt, std::span<const std::byte> input,
    std::span<std::byte> output) {
    auto hash = hash_bytes(1469598103934665603ULL, salt);
    fill_expansion(hash_bytes(hash, input), output);
}

void deterministic_expand(void*, ruvia::quic_cipher_suite,
    std::span<const std::byte> secret, std::span<const std::byte> info,
    std::span<std::byte> output) {
    auto hash = hash_bytes(1469598103934665603ULL, secret);
    fill_expansion(hash_bytes(hash, info), output);
}

struct deterministic_aead final {
    std::array<std::byte, 32> key{};
    std::size_t key_size{};
};

std::uint64_t aead_hash(const deterministic_aead& key,
    std::span<const std::byte, 12> nonce, std::span<const std::byte> aad,
    std::span<const std::byte> ciphertext) noexcept {
    auto hash = hash_bytes(1469598103934665603ULL, std::span(key.key).first(key.key_size));
    hash = hash_bytes(hash, nonce);
    hash = hash_bytes(hash, aad);
    return hash_bytes(hash, ciphertext);
}

void deterministic_seal(void* opaque, std::span<const std::byte, 12> nonce,
    std::span<const std::byte> aad, std::span<const std::byte> plaintext,
    std::span<std::byte> output) {
    const auto& key = *static_cast<deterministic_aead*>(opaque);
    if (output.size() < plaintext.size() + 16) {
        throw std::invalid_argument("deterministic test AEAD output is too small");
    }
    for (std::size_t i = 0; i < plaintext.size(); ++i) {
        output[i] = plaintext[i] ^ key.key[(i + std::to_integer<unsigned char>(nonce[i % nonce.size()])) % key.key_size];
    }
    const auto hash = aead_hash(key, nonce, aad, output.first(plaintext.size()));
    for (std::size_t i = 0; i < 16; ++i) {
        output[plaintext.size() + i] = static_cast<std::byte>(hash >> ((i % 8) * 8));
    }
}

ruvia::quic_aead_key_operations::open_result deterministic_open(void* opaque,
    std::span<const std::byte, 12> nonce, std::span<const std::byte> aad,
    std::span<const std::byte> input, std::span<std::byte> plaintext) {
    using result = ruvia::quic_aead_key_operations::open_result;
    const auto& key = *static_cast<deterministic_aead*>(opaque);
    if (input.size() < 16 || input.size() - 16 > plaintext.size()) {
        return {result::status::rejected, 0};
    }
    const auto ciphertext = input.first(input.size() - 16);
    const auto hash = aead_hash(key, nonce, aad, ciphertext);
    for (std::size_t i = 0; i < 16; ++i) {
        if (input[ciphertext.size() + i] != static_cast<std::byte>(hash >> ((i % 8) * 8))) {
            return {result::status::rejected, 0};
        }
    }
    for (std::size_t i = 0; i < ciphertext.size(); ++i) {
        plaintext[i] = ciphertext[i] ^ key.key[(i + std::to_integer<unsigned char>(nonce[i % nonce.size()])) % key.key_size];
    }
    return {result::status::authenticated, ciphertext.size()};
}

void delete_deterministic_aead(void* opaque) noexcept {
    delete static_cast<deterministic_aead*>(opaque);
}

ruvia::quic_aead_key deterministic_aead_key(void*, ruvia::quic_cipher_suite,
    ruvia::quic_crypto_direction, std::span<const std::byte> bytes) {
    if (bytes.empty() || bytes.size() > deterministic_aead{}.key.size()) {
        throw std::invalid_argument("invalid deterministic test AEAD key size");
    }
    auto* key = new deterministic_aead;
    std::ranges::copy(bytes, key->key.begin());
    key->key_size = bytes.size();
    return ruvia::quic_aead_key::adopt(key, {.destroy = delete_deterministic_aead,
                                                .seal = deterministic_seal,
                                                .open = deterministic_open});
}

void deterministic_mask(void*, std::span<const std::byte, 16>, std::span<std::byte, 5> output) {
    std::ranges::fill(output, std::byte{});
}

void delete_deterministic_header(void* opaque) noexcept {
    delete static_cast<std::byte*>(opaque);
}

ruvia::quic_header_protection_key deterministic_header_key(void*,
    ruvia::quic_cipher_suite, std::span<const std::byte>) {
    return ruvia::quic_header_protection_key::adopt(new std::byte{}, {.destroy = delete_deterministic_header,
                                                                         .mask = deterministic_mask});
}

void deterministic_erase(void*, std::span<std::byte> bytes) noexcept {
    std::ranges::fill(bytes, std::byte{});
}

class failing_memory_resource final : public std::pmr::memory_resource {
public:
    std::size_t allocations_before_failure{std::numeric_limits<std::size_t>::max()};

private:
    void* do_allocate(std::size_t size, std::size_t alignment) override {
        // Leave noexcept debug iterator metadata allocations available.
        // Pending packet and index storage are the fallible allocations.
        if (size >= 32 && allocations_before_failure == 0) {
            allocations_before_failure = std::numeric_limits<std::size_t>::max();
            throw std::bad_alloc();
        }
        if (size >= 32 && allocations_before_failure != std::numeric_limits<std::size_t>::max()) {
            --allocations_before_failure;
        }
        return std::pmr::new_delete_resource()->allocate(size, alignment);
    }

    void do_deallocate(void* pointer, std::size_t size, std::size_t alignment) override {
        std::pmr::new_delete_resource()->deallocate(pointer, size, alignment);
    }

    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

ruvia::quic_crypto_provider_view deterministic_provider(deterministic_crypto& context) {
    return {.context = &context,
        .random_bytes = deterministic_random,
        .hkdf_extract = deterministic_extract,
        .hkdf_expand = deterministic_expand,
        .create_aead_key = deterministic_aead_key,
        .create_header_protection_key = deterministic_header_key,
        .secure_erase = deterministic_erase};
}

ruvia::quic_tls_drive_result test_tls_drive(void*, ruvia::quic_tls_handshake&) noexcept {
    return {};
}

void test_tls_retire(void*) noexcept {}

ruvia::quic_tls_driver_view test_tls_driver() {
    return {.drive = test_tls_drive, .retire = test_tls_retire};
}

struct fake_tls_pair final {
    std::vector<std::byte> client_parameters;
    std::vector<std::byte> server_parameters;
};

struct fake_tls_endpoint final {
    fake_tls_pair* pair{};
    bool client{};
    bool emitted_initial_crypto{};
    bool completed{};
};

ruvia::quic_tls_drive_result drive_fake_tls(void* opaque,
    ruvia::quic_tls_handshake& handshake) noexcept {
    auto& endpoint = *static_cast<fake_tls_endpoint*>(opaque);
    auto& own_parameters = endpoint.client ? endpoint.pair->client_parameters
                                           : endpoint.pair->server_parameters;
    auto& peer_parameters = endpoint.client ? endpoint.pair->server_parameters
                                            : endpoint.pair->client_parameters;
    try {
        if (own_parameters.empty()) {
            const auto local_parameters = handshake.local_transport_parameters();
            own_parameters.assign(local_parameters.begin(), local_parameters.end());
        }
        if (endpoint.client && !endpoint.emitted_initial_crypto) {
            constexpr std::array client_hello{
                std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04}};
            if (handshake.submit_crypto(ruvia::quic_encryption_level::initial, client_hello) !=
                ruvia::quic_operation_status::accepted) {
                return {.progress = ruvia::quic_tls_progress::failed};
            }
            endpoint.emitted_initial_crypto = true;
        }
        auto record = handshake.take_crypto_record();
        if (!record || endpoint.completed || peer_parameters.empty()) {
            return {};
        }
        record.consume(record.bytes().size());
        handshake.submit_peer_transport_parameters(peer_parameters);
        constexpr std::array secret{
            std::byte{0x31}, std::byte{0x32}, std::byte{0x33}, std::byte{0x34},
            std::byte{0x35}, std::byte{0x36}, std::byte{0x37}, std::byte{0x38},
            std::byte{0x39}, std::byte{0x3a}, std::byte{0x3b}, std::byte{0x3c},
            std::byte{0x3d}, std::byte{0x3e}, std::byte{0x3f}, std::byte{0x40},
            std::byte{0x41}, std::byte{0x42}, std::byte{0x43}, std::byte{0x44},
            std::byte{0x45}, std::byte{0x46}, std::byte{0x47}, std::byte{0x48},
            std::byte{0x49}, std::byte{0x4a}, std::byte{0x4b}, std::byte{0x4c},
            std::byte{0x4d}, std::byte{0x4e}, std::byte{0x4f}, std::byte{0x50}};
        for (const auto level : {ruvia::quic_encryption_level::handshake,
                 ruvia::quic_encryption_level::application}) {
            handshake.submit_secret(level, ruvia::quic_crypto_direction::read,
                ruvia::quic_cipher_suite::aes_128_gcm_sha256, secret);
            handshake.submit_secret(level, ruvia::quic_crypto_direction::write,
                ruvia::quic_cipher_suite::aes_128_gcm_sha256, secret);
        }
        if (!endpoint.client) {
            constexpr std::array server_hello{
                std::byte{0x05}, std::byte{0x06}, std::byte{0x07}, std::byte{0x08}};
            (void)handshake.submit_crypto(ruvia::quic_encryption_level::initial, server_hello);
        }
        constexpr std::array alpn{std::byte{'h'}, std::byte{'3'}};
        handshake.complete({.negotiated_alpn = alpn,
            .cipher_suite = ruvia::quic_cipher_suite::aes_128_gcm_sha256});
        endpoint.completed = true;
        return {.progress = ruvia::quic_tls_progress::completed};
    } catch (...) {
        return {.progress = ruvia::quic_tls_progress::failed};
    }
}

ruvia::quic_tls_driver_view fake_tls_driver(fake_tls_endpoint& endpoint) {
    return {.context = &endpoint, .drive = drive_fake_tls, .retire = test_tls_retire};
}

std::array<std::byte, 1200> initialPacket() {
    std::array<std::byte, 1200> packet{};
    packet[0] = std::byte{0xc0};
    packet[4] = std::byte{1};
    packet[5] = std::byte{8};
    for (std::size_t i = 0; i < 8; ++i) {
        packet[6 + i] = static_cast<std::byte>(0x10 + i);
    }
    packet[14] = std::byte{8};
    for (std::size_t i = 0; i < 8; ++i) {
        packet[15 + i] = static_cast<std::byte>(0x20 + i);
    }
    packet[23] = std::byte{0};
    packet[24] = std::byte{0x44};
    packet[25] = std::byte{0x96};
    return packet;
}

ruvia::quic_address testAddress(std::uint16_t port) {
    ruvia::quic_address result;
    result.family = ruvia::quic_address_family::ipv4;
    result.port = port;
    result.bytes[0] = std::byte{127};
    result.bytes[3] = std::byte{1};
    return result;
}

std::array<std::byte, 64> cidProbePacket(const ruvia::quic_connection_id& destination) {
    std::array<std::byte, 64> packet{};
    packet[0] = std::byte{0xc0};
    packet[4] = std::byte{1};
    packet[5] = static_cast<std::byte>(destination.size());
    std::ranges::copy(destination.view(), packet.begin() + 6);
    const auto scid_length_offset = 6 + destination.size();
    packet[scid_length_offset] = std::byte{8};
    for (std::size_t i = 0; i < 8; ++i) {
        packet[scid_length_offset + 1 + i] = static_cast<std::byte>(0x60 + i);
    }
    return packet;
}

struct client_initial_packet final {
    std::array<std::byte, 2048> bytes{};
    std::size_t size{};
};

client_initial_packet create_client_initial(std::uint8_t cid_prefix,
    deterministic_crypto& crypto, std::pmr::memory_resource* resource) {
    const auto local = testAddress(43001);
    const auto peer = testAddress(4433);
    std::array<std::byte, 8> destination{};
    std::array<std::byte, 8> source{};
    for (std::size_t i = 0; i < destination.size(); ++i) {
        destination[i] = static_cast<std::byte>(cid_prefix + i);
        source[i] = static_cast<std::byte>(cid_prefix + 0x20 + i);
    }
    ruvia::quic_connection_config config;
    config.role = ruvia::quic_role::client;
    config.local_address = local;
    config.peer_address = peer;
    config.destination_connection_id = ruvia::quic_connection_id(destination);
    config.source_connection_id = ruvia::quic_connection_id(source);
    fake_tls_pair tls_pair;
    fake_tls_endpoint tls_endpoint{.pair = &tls_pair, .client = true};
    ruvia::quic_connection client(config, deterministic_provider(crypto),
        fake_tls_driver(tls_endpoint), resource, {});
    client_initial_packet packet;
    const auto result = client.write_packet(packet.bytes, {});
    if (result.status != ruvia::quic_operation_status::accepted) {
        throw std::runtime_error("deterministic test client failed to write an Initial packet");
    }
    packet.size = result.size;
    return packet;
}

std::array<std::byte, 1200> unsupportedVersionPacket() {
    std::array<std::byte, 1200> packet{};
    packet[0] = std::byte{0xc0};
    packet[1] = std::byte{0xfa};
    packet[2] = std::byte{0xce};
    packet[3] = std::byte{0xb0};
    packet[4] = std::byte{0x0c};
    packet[5] = std::byte{8};
    for (std::size_t i = 0; i < 8; ++i) {
        packet[6 + i] = static_cast<std::byte>(0x10 + i);
    }
    packet[14] = std::byte{8};
    for (std::size_t i = 0; i < 8; ++i) {
        packet[15 + i] = static_cast<std::byte>(0x20 + i);
    }
    return packet;
}

RUVIA_TEST(quic_server_emits_one_shot_version_negotiation_with_reversed_cids) {
    std::pmr::monotonic_buffer_resource memory;
    ruvia::quic_server server({}, provider(), &memory);
    auto packet = unsupportedVersionPacket();
    ruvia::quic_datagram_view datagram{.bytes = packet};
    auto route = server.route_datagram(datagram);
    RUVIA_CHECK_EQ(route.kind, ruvia::quic_server_route_kind::version_negotiation);
    RUVIA_CHECK(route.version_negotiation.valid());

    std::array<std::byte, 1200> output{};
    auto blocked = server.write_version_negotiation(route.version_negotiation,
        std::span(output).first(1));
    RUVIA_CHECK_EQ(blocked.status, ruvia::quic_operation_status::would_block);
    RUVIA_CHECK(route.version_negotiation.valid());

    const auto written = server.write_version_negotiation(route.version_negotiation, output);
    RUVIA_CHECK_EQ(written.status, ruvia::quic_operation_status::accepted);
    RUVIA_CHECK(written.size <= packet.size() * 3);
    RUVIA_CHECK(!route.version_negotiation.valid());

    ngtcp2_version_cid decoded{};
    const auto result = ngtcp2_pkt_decode_version_cid(&decoded,
        reinterpret_cast<const std::uint8_t*>(output.data()), written.size, 16);
    RUVIA_CHECK_EQ(result, 0);
    RUVIA_CHECK_EQ(decoded.version, std::uint32_t{0});
    RUVIA_CHECK_EQ(decoded.dcidlen, std::size_t{8});
    RUVIA_CHECK_EQ(decoded.scidlen, std::size_t{8});
    RUVIA_CHECK(std::equal(decoded.dcid, decoded.dcid + decoded.dcidlen,
        reinterpret_cast<const std::uint8_t*>(packet.data() + 15)));
    RUVIA_CHECK(std::equal(decoded.scid, decoded.scid + decoded.scidlen,
        reinterpret_cast<const std::uint8_t*>(packet.data() + 6)));
    RUVIA_CHECK_EQ(written.size, std::size_t{31});
    constexpr std::array<std::byte, 8> advertised_versions{
        std::byte{0}, std::byte{0}, std::byte{0}, std::byte{1},
        std::byte{0x6b}, std::byte{0x33}, std::byte{0x43}, std::byte{0xcf}};
    RUVIA_CHECK(std::equal(advertised_versions.begin(), advertised_versions.end(),
        output.begin() + 23));

    bool rejected_repeat{};
    try {
        (void)server.write_version_negotiation(route.version_negotiation, output);
    } catch (const std::invalid_argument&) {
        rejected_repeat = true;
    }
    RUVIA_CHECK(rejected_repeat);
}

RUVIA_TEST(quic_server_owns_one_pending_initial_and_obeys_pending_byte_budget) {
    std::pmr::monotonic_buffer_resource memory;
    ruvia::quic_server server({}, provider(), &memory);
    auto packet = initialPacket();
    auto route = server.route_datagram({.bytes = packet});
    RUVIA_CHECK_EQ(route.kind, ruvia::quic_server_route_kind::initial_offer);
    RUVIA_CHECK_EQ(server.pending_connection_count(), std::size_t{1});
    auto duplicate = server.route_datagram({.bytes = packet});
    RUVIA_CHECK_EQ(duplicate.kind, ruvia::quic_server_route_kind::initial_offer);
    RUVIA_CHECK_EQ(duplicate.offer.offer_id, route.offer.offer_id);
    RUVIA_CHECK_EQ(server.pending_connection_count(), std::size_t{1});

    packet[15] = std::byte{0x7f};
    RUVIA_CHECK_EQ(server.route_datagram({.bytes = packet}).kind,
        ruvia::quic_server_route_kind::dropped);

    ruvia::quic_server_config no_pending_config;
    no_pending_config.max_pending_datagram_bytes = 0;
    ruvia::quic_server no_pending(no_pending_config, provider(), &memory);
    packet = initialPacket();
    const auto rejected = no_pending.route_datagram({.bytes = packet});
    RUVIA_CHECK_EQ(rejected.kind, ruvia::quic_server_route_kind::rejected);
    RUVIA_CHECK_EQ(rejected.status, ruvia::quic_operation_status::would_block);
    RUVIA_CHECK_EQ(no_pending.pending_connection_count(), std::size_t{0});
}

RUVIA_TEST(quic_server_rejects_forged_or_stale_initial_offer_without_consuming_pending_state) {
    std::pmr::monotonic_buffer_resource memory;
    ruvia::quic_server server({}, provider(), &memory);
    auto packet = initialPacket();
    const auto route = server.route_datagram({.bytes = packet});
    auto forged = route.offer;
    ++forged.offer_id;
    bool rejected{};
    try {
        (void)server.admit_initial(forged, {}, {});
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
    RUVIA_CHECK_EQ(server.pending_connection_count(), std::size_t{1});

    forged = route.offer;
    forged.peer_address.port = 1234;
    rejected = false;
    try {
        (void)server.admit_initial(forged, {}, {});
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
    RUVIA_CHECK_EQ(server.pending_connection_count(), std::size_t{1});
}

RUVIA_TEST(quic_server_vn_rng_failure_keeps_plan_valid_and_output_untouched) {
    std::pmr::monotonic_buffer_resource memory;
    ruvia::quic_server server({}, provider(), &memory);
    auto packet = unsupportedVersionPacket();
    auto route = server.route_datagram({.bytes = packet});
    auto plan = std::move(route.version_negotiation);
    RUVIA_CHECK(!route.version_negotiation.valid());
    RUVIA_CHECK(plan.valid());
    std::array<std::byte, 1200> output;
    std::ranges::fill(output, std::byte{0x7a});
    int failure_marker{};
    auto failing = provider();
    failing.context = &failure_marker;
    ruvia::quic_server failing_server({}, failing, &memory);
    bool threw{};
    try {
        (void)failing_server.write_version_negotiation(plan, output);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    RUVIA_CHECK(threw);
    RUVIA_CHECK(plan.valid());
    RUVIA_CHECK(std::ranges::all_of(output, [](std::byte value) { return value == std::byte{0x7a}; }));
}

RUVIA_TEST(quic_server_drops_small_or_truncated_unknown_version_packets) {
    std::pmr::monotonic_buffer_resource memory;
    ruvia::quic_server server({}, provider(), &memory);
    auto packet = unsupportedVersionPacket();
    ruvia::quic_datagram_view datagram{.bytes = std::span(packet).first(1199)};
    RUVIA_CHECK_EQ(server.route_datagram(datagram).kind,
        ruvia::quic_server_route_kind::dropped);
    datagram.bytes = std::span(packet).first(5);
    RUVIA_CHECK_EQ(server.route_datagram(datagram).kind,
        ruvia::quic_server_route_kind::dropped);
    RUVIA_CHECK_EQ(server.pending_connection_count(), std::size_t{0});
    RUVIA_CHECK_EQ(server.connection_count(), std::size_t{0});
}

RUVIA_TEST(quic_server_rejects_invalid_transport_parameter_configuration) {
    std::pmr::monotonic_buffer_resource memory;
    ruvia::quic_server_config config;
    config.local_transport_parameters.max_udp_payload_size = 1199;
    bool rejected{};
    try {
        ruvia::quic_server server(config, provider(), &memory);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
}

RUVIA_TEST(quic_server_admission_consumes_cached_initial_and_routes_published_cids) {
    std::pmr::unsynchronized_pool_resource memory;
    deterministic_crypto client_crypto;
    deterministic_crypto server_crypto;
    fake_tls_pair tls_pair;
    fake_tls_endpoint client_tls{.pair = &tls_pair, .client = true};
    fake_tls_endpoint server_tls{.pair = &tls_pair, .client = false};
    ruvia::quic_server server({}, deterministic_provider(server_crypto), &memory);
    const auto client_address = testAddress(43001);
    const auto server_address = testAddress(4433);
    std::array<std::byte, 8> initial_dcid{};
    std::array<std::byte, 8> client_scid{};
    for (std::size_t i = 0; i < initial_dcid.size(); ++i) {
        initial_dcid[i] = static_cast<std::byte>(0x10 + i);
        client_scid[i] = static_cast<std::byte>(0x30 + i);
    }
    ruvia::quic_connection_config client_config;
    client_config.role = ruvia::quic_role::client;
    client_config.local_address = client_address;
    client_config.peer_address = server_address;
    client_config.destination_connection_id = ruvia::quic_connection_id(initial_dcid);
    client_config.source_connection_id = ruvia::quic_connection_id(client_scid);
    ruvia::quic_connection client(client_config, deterministic_provider(client_crypto),
        fake_tls_driver(client_tls), &memory, {});
    std::array<std::byte, 2048> initial_bytes{};
    const auto client_initial = client.write_packet(initial_bytes, {});
    RUVIA_CHECK_EQ(client_initial.status, ruvia::quic_operation_status::accepted);
    const auto datagram = ruvia::quic_datagram_view{
        .bytes = std::span(initial_bytes).first(client_initial.size),
        .local = server_address,
        .peer = client_address};
    const auto offer = server.route_datagram(datagram);
    RUVIA_CHECK_EQ(offer.kind, ruvia::quic_server_route_kind::initial_offer);
    const auto duplicate = server.route_datagram(datagram);
    RUVIA_CHECK_EQ(duplicate.kind, ruvia::quic_server_route_kind::initial_offer);
    RUVIA_CHECK_EQ(duplicate.offer.offer_id, offer.offer.offer_id);

    auto changed_peer_scid = initial_bytes;
    changed_peer_scid[15] ^= std::byte{0x01};
    RUVIA_CHECK_EQ(server.route_datagram({.bytes = std::span(changed_peer_scid).first(client_initial.size),
                                             .local = server_address,
                                             .peer = client_address})
                       .kind,
        ruvia::quic_server_route_kind::dropped);
    RUVIA_CHECK_EQ(server.pending_connection_count(), std::size_t{1});

    const auto admitted = server.admit_initial(offer.offer, fake_tls_driver(server_tls), {});
    RUVIA_CHECK_EQ(admitted.status, ruvia::quic_operation_status::accepted);
    RUVIA_CHECK_EQ(server.pending_connection_count(), std::size_t{0});
    RUVIA_CHECK_EQ(server.connection_count(), std::size_t{1});
    bool stale_offer_rejected{};
    try {
        (void)server.admit_initial(offer.offer, test_tls_driver(), {});
    } catch (const std::invalid_argument&) {
        stale_offer_rejected = true;
    }
    RUVIA_CHECK(stale_offer_rejected);

    auto now = ruvia::quic_timestamp{};
    for (std::size_t attempt = 0; attempt < 8; ++attempt) {
        now += std::chrono::milliseconds(1);
        std::array<std::byte, 2048> server_packet{};
        const auto server_result = server.connection(admitted.connection).write_packet(server_packet, now);
        if (server_result.size != 0) {
            const ruvia::quic_datagram_view server_datagram{
                .bytes = std::span(server_packet).first(server_result.size),
                .local = server_address,
                .peer = client_address};
            (void)client.receive(server_datagram, now);
        }
        std::array<std::byte, 2048> client_packet{};
        const auto client_result = client.write_packet(client_packet, now);
        if (client_result.size != 0) {
            const ruvia::quic_datagram_view client_datagram{
                .bytes = std::span(client_packet).first(client_result.size),
                .local = client_address,
                .peer = server_address};
            (void)server.receive(admitted.connection, client_datagram, now);
        }
    }
    RUVIA_CHECK(server_tls.completed);
    RUVIA_CHECK(!server_crypto.random_outputs.empty());
    if (server_crypto.random_outputs.empty()) {
        return;
    }
    const ruvia::quic_connection_id server_scid(server_crypto.random_outputs.front());
    auto route_cid = [&](const ruvia::quic_connection_id& cid) {
        const auto probe = cidProbePacket(cid);
        return server.route_datagram({.bytes = probe, .local = server_address, .peer = client_address});
    };
    RUVIA_CHECK_EQ(route_cid(offer.offer.original_destination_connection_id).kind,
        ruvia::quic_server_route_kind::existing_connection);
    RUVIA_CHECK_EQ(route_cid(server_scid).kind,
        ruvia::quic_server_route_kind::existing_connection);

    std::array<std::byte, 32> short_packet{};
    short_packet.front() = std::byte{0x40};
    std::ranges::copy(server_scid.view(), short_packet.begin() + 1);
    const auto short_route = server.route_datagram({.bytes = short_packet,
        .local = server_address,
        .peer = client_address});
    RUVIA_CHECK_EQ(short_route.kind, ruvia::quic_server_route_kind::existing_connection);
    RUVIA_CHECK_EQ(short_route.connection, admitted.connection);
    short_packet[1] ^= std::byte{0xff};
    RUVIA_CHECK_EQ(server.route_datagram({.bytes = short_packet}).kind,
        ruvia::quic_server_route_kind::dropped);

    // Millisecond configuration must survive ngtcp2's nanosecond duration API.
    now += std::chrono::milliseconds(10);
    (void)server.connection(admitted.connection).handle_expiry(now);
    RUVIA_CHECK(server.connection(admitted.connection).info().state !=
                ruvia::quic_connection_state::retired);

    bool native_cid_routed{};
    for (std::size_t i = 1; i < server_crypto.random_outputs.size(); ++i) {
        const auto& bytes = server_crypto.random_outputs[i];
        if (bytes.size() < 8 || bytes.size() > ruvia::quic_max_connection_id_size) {
            continue;
        }
        const ruvia::quic_connection_id candidate(bytes);
        if (candidate == server_scid || candidate == offer.offer.original_destination_connection_id) {
            continue;
        }
        const auto routed = route_cid(candidate);
        native_cid_routed |= routed.kind == ruvia::quic_server_route_kind::existing_connection &&
                             routed.connection == admitted.connection;
    }
    RUVIA_CHECK(native_cid_routed);

    RUVIA_CHECK_EQ(server.retire(admitted.connection), ruvia::quic_operation_status::retired);
    RUVIA_CHECK_EQ(server.connection_count(), std::size_t{0});
    RUVIA_CHECK_EQ(server.retire(admitted.connection), ruvia::quic_operation_status::retired);
    bool stale_token_rejected{};
    try {
        (void)server.connection(admitted.connection);
    } catch (const std::out_of_range&) {
        stale_token_rejected = true;
    }
    RUVIA_CHECK(stale_token_rejected);
    RUVIA_CHECK_EQ(route_cid(server_scid).kind, ruvia::quic_server_route_kind::dropped);
    RUVIA_CHECK_EQ(route_cid(offer.offer.original_destination_connection_id).kind,
        ruvia::quic_server_route_kind::dropped);
    for (std::size_t i = 1; i < server_crypto.random_outputs.size(); ++i) {
        const auto& bytes = server_crypto.random_outputs[i];
        if (bytes.size() < 8 || bytes.size() > ruvia::quic_max_connection_id_size) {
            continue;
        }
        const ruvia::quic_connection_id candidate(bytes);
        RUVIA_CHECK_EQ(route_cid(candidate).kind, ruvia::quic_server_route_kind::dropped);
    }
}

RUVIA_TEST(quic_server_pending_initial_allocation_failures_leave_no_cached_state) {
    auto packet = initialPacket();
    std::size_t exercised_failures{};
    for (std::size_t fail_after = 0; fail_after < 6; ++fail_after) {
        failing_memory_resource memory;
        ruvia::quic_server server({}, provider(), &memory);
        memory.allocations_before_failure = fail_after;
        bool allocation_failed{};
        try {
            (void)server.route_datagram({.bytes = packet});
        } catch (const std::bad_alloc&) {
            allocation_failed = true;
        }
        if (allocation_failed) {
            ++exercised_failures;
            RUVIA_CHECK_EQ(server.pending_connection_count(), std::size_t{0});
            memory.allocations_before_failure = std::numeric_limits<std::size_t>::max();
            const auto retried = server.route_datagram({.bytes = packet});
            RUVIA_CHECK_EQ(retried.kind, ruvia::quic_server_route_kind::initial_offer);
            RUVIA_CHECK_EQ(server.pending_connection_count(), std::size_t{1});
        } else {
            RUVIA_CHECK_EQ(server.pending_connection_count(), std::size_t{1});
        }
    }
    RUVIA_CHECK(exercised_failures >= 3);
}

RUVIA_TEST(quic_server_cid_collision_rolls_back_admission_and_preserves_existing_owner) {
    std::pmr::unsynchronized_pool_resource memory;
    deterministic_crypto server_crypto;
    ruvia::quic_server server({}, deterministic_provider(server_crypto), &memory);
    deterministic_crypto first_client_crypto;
    auto first = create_client_initial(0x10, first_client_crypto, &memory);
    const auto local = testAddress(4433);
    const auto peer = testAddress(43001);
    const auto first_offer = server.route_datagram({.bytes = std::span(first.bytes).first(first.size), .local = local, .peer = peer});
    const auto first_admitted = server.admit_initial(first_offer.offer, test_tls_driver(), {});
    RUVIA_CHECK_EQ(first_admitted.status, ruvia::quic_operation_status::accepted);
    RUVIA_CHECK(!server_crypto.random_outputs.empty());
    if (server_crypto.random_outputs.empty()) {
        return;
    }
    const ruvia::quic_connection_id existing_cid(server_crypto.random_outputs.front());
    auto probe = cidProbePacket(existing_cid);
    const auto existing_route = server.route_datagram({.bytes = probe, .local = local, .peer = peer});
    RUVIA_CHECK_EQ(existing_route.kind, ruvia::quic_server_route_kind::existing_connection);
    RUVIA_CHECK_EQ(existing_route.connection, first_admitted.connection);

    deterministic_crypto second_client_crypto;
    auto second = create_client_initial(0x30, second_client_crypto, &memory);
    const auto second_offer = server.route_datagram({.bytes = std::span(second.bytes).first(second.size), .local = local, .peer = peer});
    RUVIA_CHECK_EQ(second_offer.kind, ruvia::quic_server_route_kind::initial_offer);
    server_crypto.forced_random = std::vector<std::byte>(
        existing_cid.view().begin(), existing_cid.view().end());
    bool collision_failed{};
    try {
        (void)server.admit_initial(second_offer.offer, test_tls_driver(), {});
    } catch (const std::runtime_error&) {
        collision_failed = true;
    }
    RUVIA_CHECK(collision_failed);
    RUVIA_CHECK_EQ(server.connection_count(), std::size_t{1});
    RUVIA_CHECK_EQ(server.pending_connection_count(), std::size_t{1});
    const auto after_failure = server.route_datagram({.bytes = probe, .local = local, .peer = peer});
    RUVIA_CHECK_EQ(after_failure.kind, ruvia::quic_server_route_kind::existing_connection);
    RUVIA_CHECK_EQ(after_failure.connection, first_admitted.connection);

    server_crypto.forced_random.reset();
    const auto second_admitted = server.admit_initial(second_offer.offer, test_tls_driver(), {});
    RUVIA_CHECK_EQ(second_admitted.status, ruvia::quic_operation_status::accepted);
    RUVIA_CHECK_EQ(server.connection_count(), std::size_t{2});
}

}  // namespace
