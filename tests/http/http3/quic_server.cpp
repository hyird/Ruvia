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

void random_bytes(void* context_value, std::span<std::byte> output) {
    if (context_value) {
        throw std::runtime_error("controlled random provider failure");
    }
    static std::uint8_t next_value = 0x41;
    for (auto& byte : output) {
        byte = static_cast<std::byte>(next_value++);
    }
}

void hkdf_extract(void*, ruvia::quic_cipher_suite, std::span<const std::byte>,
    std::span<const std::byte>, std::span<std::byte>) {}
void hkdf_expand(void*, ruvia::quic_cipher_suite, std::span<const std::byte>,
    std::span<const std::byte>, std::span<std::byte>) {}
ruvia::quic_aead_key make_aead(void*, ruvia::quic_cipher_suite,
    ruvia::quic_crypto_direction, std::span<const std::byte>) {
    return {};
}
ruvia::quic_header_protection_key make_header_key(void*, ruvia::quic_cipher_suite,
    std::span<const std::byte>) {
    return {};
}
void erase_bytes(void*, std::span<std::byte>) noexcept {}

ruvia::quic_crypto_provider_view provider() {
    return {.random_bytes_ = random_bytes,
        .hkdf_extract_ = hkdf_extract,
        .hkdf_expand_ = hkdf_expand,
        .create_aead_key_ = make_aead,
        .create_header_protection_key_ = make_header_key,
        .secure_erase_ = erase_bytes};
}

struct deterministic_crypto final {
    std::uint8_t next_{0x80};
    std::vector<std::vector<std::byte>> random_outputs_;
    std::optional<std::vector<std::byte>> forced_random_;
};

std::uint64_t hash_bytes(std::uint64_t hash, std::span<const std::byte> bytes_value) noexcept {
    for (const auto byte : bytes_value) {
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
    if (crypto.forced_random_ && crypto.forced_random_->size() == output.size()) {
        std::ranges::copy(*crypto.forced_random_, output.begin());
    } else {
        for (auto& byte : output) {
            byte = static_cast<std::byte>(crypto.next_++);
        }
    }
    crypto.random_outputs_.emplace_back(output.begin(), output.end());
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
    std::array<std::byte, 32> key_{};
    std::size_t key_size_{};
};

std::uint64_t aead_hash(const deterministic_aead& key,
    std::span<const std::byte, 12> nonce, std::span<const std::byte> aad,
    std::span<const std::byte> ciphertext) noexcept {
    auto hash = hash_bytes(1469598103934665603ULL, std::span(key.key_).first(key.key_size_));
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
        output[i] = plaintext[i] ^ key.key_[(i + std::to_integer<unsigned char>(nonce[i % nonce.size()])) % key.key_size_];
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
        plaintext[i] = ciphertext[i] ^ key.key_[(i + std::to_integer<unsigned char>(nonce[i % nonce.size()])) % key.key_size_];
    }
    return {result::status::authenticated, ciphertext.size()};
}

void delete_deterministic_aead(void* opaque) noexcept {
    delete static_cast<deterministic_aead*>(opaque);
}

ruvia::quic_aead_key deterministic_aead_key(void*, ruvia::quic_cipher_suite,
    ruvia::quic_crypto_direction, std::span<const std::byte> bytes_value) {
    if (bytes_value.empty() || bytes_value.size() > deterministic_aead{}.key_.size()) {
        throw std::invalid_argument("invalid deterministic test AEAD key size");
    }
    auto* key = new deterministic_aead;
    std::ranges::copy(bytes_value, key->key_.begin());
    key->key_size_ = bytes_value.size();
    return ruvia::quic_aead_key::adopt(key, {.destroy_ = delete_deterministic_aead,
                                                .seal_ = deterministic_seal,
                                                .open_ = deterministic_open});
}

void deterministic_mask(void*, std::span<const std::byte, 16>, std::span<std::byte, 5> output) {
    std::ranges::fill(output, std::byte{});
}

void delete_deterministic_header(void* opaque) noexcept {
    delete static_cast<std::byte*>(opaque);
}

ruvia::quic_header_protection_key deterministic_header_key(void*,
    ruvia::quic_cipher_suite, std::span<const std::byte>) {
    return ruvia::quic_header_protection_key::adopt(new std::byte{}, {.destroy_ = delete_deterministic_header,
                                                                         .mask_ = deterministic_mask});
}

void deterministic_erase(void*, std::span<std::byte> bytes_value) noexcept {
    std::ranges::fill(bytes_value, std::byte{});
}

class failing_memory_resource final : public std::pmr::memory_resource {
public:
    std::size_t allocations_before_failure_{std::numeric_limits<std::size_t>::max()};

private:
    void* do_allocate(std::size_t size, std::size_t alignment) override {
        // Leave noexcept debug iterator metadata allocations available.
        // Pending packet and index storage are the fallible allocations.
        if (size >= 32 && allocations_before_failure_ == 0) {
            allocations_before_failure_ = std::numeric_limits<std::size_t>::max();
            throw std::bad_alloc();
        }
        if (size >= 32 && allocations_before_failure_ != std::numeric_limits<std::size_t>::max()) {
            --allocations_before_failure_;
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

ruvia::quic_crypto_provider_view deterministic_provider(deterministic_crypto& context_value) {
    return {.context_ = &context_value,
        .random_bytes_ = deterministic_random,
        .hkdf_extract_ = deterministic_extract,
        .hkdf_expand_ = deterministic_expand,
        .create_aead_key_ = deterministic_aead_key,
        .create_header_protection_key_ = deterministic_header_key,
        .secure_erase_ = deterministic_erase};
}

ruvia::quic_tls_drive_result test_tls_drive(void*, ruvia::quic_tls_handshake&) noexcept {
    return {};
}

void test_tls_retire(void*) noexcept {}

ruvia::quic_tls_driver_view test_tls_driver() {
    return {.drive_ = test_tls_drive, .retire_ = test_tls_retire};
}

struct fake_tls_pair final {
    std::vector<std::byte> client_parameters_;
    std::vector<std::byte> server_parameters_;
};

struct fake_tls_endpoint final {
    fake_tls_pair* pair_{};
    bool client_{};
    bool emitted_initial_crypto_{};
    bool completed_{};
};

ruvia::quic_tls_drive_result drive_fake_tls(void* opaque,
    ruvia::quic_tls_handshake& handshake) noexcept {
    auto& endpoint = *static_cast<fake_tls_endpoint*>(opaque);
    auto& own_parameters = endpoint.client_ ? endpoint.pair_->client_parameters_
                                            : endpoint.pair_->server_parameters_;
    auto& peer_parameters = endpoint.client_ ? endpoint.pair_->server_parameters_
                                             : endpoint.pair_->client_parameters_;
    try {
        if (own_parameters.empty()) {
            const auto local_parameters = handshake.local_transport_parameters();
            own_parameters.assign(local_parameters.begin(), local_parameters.end());
        }
        if (endpoint.client_ && !endpoint.emitted_initial_crypto_) {
            constexpr std::array client_hello{
                std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04}};
            if (handshake.submit_crypto(ruvia::quic_encryption_level::initial, client_hello) !=
                ruvia::quic_operation_status::accepted) {
                return {.progress_ = ruvia::quic_tls_progress::failed};
            }
            endpoint.emitted_initial_crypto_ = true;
        }
        auto record = handshake.take_crypto_record();
        if (!record || endpoint.completed_ || peer_parameters.empty()) {
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
        if (!endpoint.client_) {
            constexpr std::array server_hello{
                std::byte{0x05}, std::byte{0x06}, std::byte{0x07}, std::byte{0x08}};
            (void)handshake.submit_crypto(ruvia::quic_encryption_level::initial, server_hello);
        }
        constexpr std::array alpn{std::byte{'h'}, std::byte{'3'}};
        handshake.complete({.negotiated_alpn_ = alpn,
            .cipher_suite_ = ruvia::quic_cipher_suite::aes_128_gcm_sha256});
        endpoint.completed_ = true;
        return {.progress_ = ruvia::quic_tls_progress::completed};
    } catch (...) {
        return {.progress_ = ruvia::quic_tls_progress::failed};
    }
}

ruvia::quic_tls_driver_view fake_tls_driver(fake_tls_endpoint& endpoint) {
    return {.context_ = &endpoint, .drive_ = drive_fake_tls, .retire_ = test_tls_retire};
}

std::array<std::byte, 1200> initial_packet() {
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

ruvia::quic_address test_address(std::uint16_t port) {
    ruvia::quic_address result;
    result.family_ = ruvia::quic_address_family::ipv4;
    result.port_ = port;
    result.bytes_[0] = std::byte{127};
    result.bytes_[3] = std::byte{1};
    return result;
}

std::array<std::byte, 64> cid_probe_packet(const ruvia::quic_connection_id& destination) {
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
    std::array<std::byte, 2048> bytes_{};
    std::size_t size_{};
};

client_initial_packet create_client_initial(std::uint8_t cid_prefix,
    deterministic_crypto& crypto, std::pmr::memory_resource* resource) {
    const auto local = test_address(43001);
    const auto peer = test_address(4433);
    std::array<std::byte, 8> destination{};
    std::array<std::byte, 8> source_value{};
    for (std::size_t i = 0; i < destination.size(); ++i) {
        destination[i] = static_cast<std::byte>(cid_prefix + i);
        source_value[i] = static_cast<std::byte>(cid_prefix + 0x20 + i);
    }
    ruvia::quic_connection_config config;
    config.role_ = ruvia::quic_role::client;
    config.local_address_ = local;
    config.peer_address_ = peer;
    config.destination_connection_id_ = ruvia::quic_connection_id(destination);
    config.source_connection_id_ = ruvia::quic_connection_id(source_value);
    fake_tls_pair tls_pair;
    fake_tls_endpoint tls_endpoint{.pair_ = &tls_pair, .client_ = true};
    ruvia::quic_connection client(config, deterministic_provider(crypto),
        fake_tls_driver(tls_endpoint), resource, {});
    client_initial_packet packet;
    const auto result_value = client.write_packet(packet.bytes_, {});
    if (result_value.status_ != ruvia::quic_operation_status::accepted) {
        throw std::runtime_error("deterministic test client failed to write an Initial packet");
    }
    packet.size_ = result_value.size_;
    return packet;
}

std::array<std::byte, 1200> unsupported_version_packet() {
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
    auto packet = unsupported_version_packet();
    ruvia::quic_datagram_view datagram{.bytes_ = packet};
    auto route = server.route_datagram(datagram);
    RUVIA_CHECK_EQ(route.kind_, ruvia::quic_server_route_kind::version_negotiation);
    RUVIA_CHECK(route.version_negotiation_.valid());

    std::array<std::byte, 1200> output{};
    auto blocked = server.write_version_negotiation(route.version_negotiation_,
        std::span(output).first(1));
    RUVIA_CHECK_EQ(blocked.status_, ruvia::quic_operation_status::would_block);
    RUVIA_CHECK(route.version_negotiation_.valid());

    const auto written = server.write_version_negotiation(route.version_negotiation_, output);
    RUVIA_CHECK_EQ(written.status_, ruvia::quic_operation_status::accepted);
    RUVIA_CHECK(written.size_ <= packet.size() * 3);
    RUVIA_CHECK(!route.version_negotiation_.valid());

    ngtcp2_version_cid decoded{};
    const auto result_value = ngtcp2_pkt_decode_version_cid(&decoded,
        reinterpret_cast<const std::uint8_t*>(output.data()), written.size_, 16);
    RUVIA_CHECK_EQ(result_value, 0);
    RUVIA_CHECK_EQ(decoded.version, std::uint32_t{0});
    RUVIA_CHECK_EQ(decoded.dcidlen, std::size_t{8});
    RUVIA_CHECK_EQ(decoded.scidlen, std::size_t{8});
    RUVIA_CHECK(std::equal(decoded.dcid, decoded.dcid + decoded.dcidlen,
        reinterpret_cast<const std::uint8_t*>(packet.data() + 15)));
    RUVIA_CHECK(std::equal(decoded.scid, decoded.scid + decoded.scidlen,
        reinterpret_cast<const std::uint8_t*>(packet.data() + 6)));
    RUVIA_CHECK_EQ(written.size_, std::size_t{31});
    constexpr std::array<std::byte, 8> advertised_versions{
        std::byte{0}, std::byte{0}, std::byte{0}, std::byte{1},
        std::byte{0x6b}, std::byte{0x33}, std::byte{0x43}, std::byte{0xcf}};
    RUVIA_CHECK(std::equal(advertised_versions.begin(), advertised_versions.end(),
        output.begin() + 23));

    bool rejected_repeat{};
    try {
        (void)server.write_version_negotiation(route.version_negotiation_, output);
    } catch (const std::invalid_argument&) {
        rejected_repeat = true;
    }
    RUVIA_CHECK(rejected_repeat);
}

RUVIA_TEST(quic_server_owns_one_pending_initial_and_obeys_pending_byte_budget) {
    std::pmr::monotonic_buffer_resource memory;
    ruvia::quic_server server({}, provider(), &memory);
    auto packet = initial_packet();
    auto route = server.route_datagram({.bytes_ = packet});
    RUVIA_CHECK_EQ(route.kind_, ruvia::quic_server_route_kind::initial_offer);
    RUVIA_CHECK_EQ(server.pending_connection_count(), std::size_t{1});
    auto duplicate = server.route_datagram({.bytes_ = packet});
    RUVIA_CHECK_EQ(duplicate.kind_, ruvia::quic_server_route_kind::initial_offer);
    RUVIA_CHECK_EQ(duplicate.offer_.offer_id_, route.offer_.offer_id_);
    RUVIA_CHECK_EQ(server.pending_connection_count(), std::size_t{1});

    packet[15] = std::byte{0x7f};
    RUVIA_CHECK_EQ(server.route_datagram({.bytes_ = packet}).kind_,
        ruvia::quic_server_route_kind::dropped);

    ruvia::quic_server_config no_pending_config;
    no_pending_config.max_pending_datagram_bytes_ = 0;
    ruvia::quic_server no_pending(no_pending_config, provider(), &memory);
    packet = initial_packet();
    const auto rejected = no_pending.route_datagram({.bytes_ = packet});
    RUVIA_CHECK_EQ(rejected.kind_, ruvia::quic_server_route_kind::rejected);
    RUVIA_CHECK_EQ(rejected.status_, ruvia::quic_operation_status::would_block);
    RUVIA_CHECK_EQ(no_pending.pending_connection_count(), std::size_t{0});
}

RUVIA_TEST(quic_server_rejects_forged_or_stale_initial_offer_without_consuming_pending_state) {
    std::pmr::monotonic_buffer_resource memory;
    ruvia::quic_server server({}, provider(), &memory);
    auto packet = initial_packet();
    const auto route = server.route_datagram({.bytes_ = packet});
    auto forged = route.offer_;
    ++forged.offer_id_;
    bool rejected{};
    try {
        (void)server.admit_initial(forged, {}, {});
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
    RUVIA_CHECK_EQ(server.pending_connection_count(), std::size_t{1});

    forged = route.offer_;
    forged.peer_address_.port_ = 1234;
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
    auto packet = unsupported_version_packet();
    auto route = server.route_datagram({.bytes_ = packet});
    auto plan = std::move(route.version_negotiation_);
    RUVIA_CHECK(!route.version_negotiation_.valid());
    RUVIA_CHECK(plan.valid());
    std::array<std::byte, 1200> output;
    std::ranges::fill(output, std::byte{0x7a});
    int failure_marker{};
    auto failing = provider();
    failing.context_ = &failure_marker;
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
    auto packet = unsupported_version_packet();
    ruvia::quic_datagram_view datagram{.bytes_ = std::span(packet).first(1199)};
    RUVIA_CHECK_EQ(server.route_datagram(datagram).kind_,
        ruvia::quic_server_route_kind::dropped);
    datagram.bytes_ = std::span(packet).first(5);
    RUVIA_CHECK_EQ(server.route_datagram(datagram).kind_,
        ruvia::quic_server_route_kind::dropped);
    RUVIA_CHECK_EQ(server.pending_connection_count(), std::size_t{0});
    RUVIA_CHECK_EQ(server.connection_count(), std::size_t{0});
}

RUVIA_TEST(quic_server_rejects_invalid_transport_parameter_configuration) {
    std::pmr::monotonic_buffer_resource memory;
    ruvia::quic_server_config config;
    config.local_transport_parameters_.max_udp_payload_size_ = 1199;
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
    fake_tls_endpoint client_tls{.pair_ = &tls_pair, .client_ = true};
    fake_tls_endpoint server_tls{.pair_ = &tls_pair, .client_ = false};
    ruvia::quic_server server({}, deterministic_provider(server_crypto), &memory);
    const auto client_address = test_address(43001);
    const auto server_address = test_address(4433);
    std::array<std::byte, 8> initial_dcid{};
    std::array<std::byte, 8> client_scid{};
    for (std::size_t i = 0; i < initial_dcid.size(); ++i) {
        initial_dcid[i] = static_cast<std::byte>(0x10 + i);
        client_scid[i] = static_cast<std::byte>(0x30 + i);
    }
    ruvia::quic_connection_config client_config;
    client_config.role_ = ruvia::quic_role::client;
    client_config.local_address_ = client_address;
    client_config.peer_address_ = server_address;
    client_config.destination_connection_id_ = ruvia::quic_connection_id(initial_dcid);
    client_config.source_connection_id_ = ruvia::quic_connection_id(client_scid);
    ruvia::quic_connection client(client_config, deterministic_provider(client_crypto),
        fake_tls_driver(client_tls), &memory, {});
    std::array<std::byte, 2048> initial_bytes{};
    const auto client_initial = client.write_packet(initial_bytes, {});
    RUVIA_CHECK_EQ(client_initial.status_, ruvia::quic_operation_status::accepted);
    const auto datagram = ruvia::quic_datagram_view{
        .bytes_ = std::span(initial_bytes).first(client_initial.size_),
        .local_ = server_address,
        .peer_ = client_address};
    const auto offer = server.route_datagram(datagram);
    RUVIA_CHECK_EQ(offer.kind_, ruvia::quic_server_route_kind::initial_offer);
    const auto duplicate = server.route_datagram(datagram);
    RUVIA_CHECK_EQ(duplicate.kind_, ruvia::quic_server_route_kind::initial_offer);
    RUVIA_CHECK_EQ(duplicate.offer_.offer_id_, offer.offer_.offer_id_);

    auto changed_peer_scid = initial_bytes;
    changed_peer_scid[15] ^= std::byte{0x01};
    RUVIA_CHECK_EQ(server.route_datagram({.bytes_ = std::span(changed_peer_scid).first(client_initial.size_),
                                             .local_ = server_address,
                                             .peer_ = client_address})
                       .kind_,
        ruvia::quic_server_route_kind::dropped);
    RUVIA_CHECK_EQ(server.pending_connection_count(), std::size_t{1});

    const auto admitted = server.admit_initial(offer.offer_, fake_tls_driver(server_tls), {});
    RUVIA_CHECK_EQ(admitted.status_, ruvia::quic_operation_status::accepted);
    RUVIA_CHECK_EQ(server.pending_connection_count(), std::size_t{0});
    RUVIA_CHECK_EQ(server.connection_count(), std::size_t{1});
    bool stale_offer_rejected{};
    try {
        (void)server.admit_initial(offer.offer_, test_tls_driver(), {});
    } catch (const std::invalid_argument&) {
        stale_offer_rejected = true;
    }
    RUVIA_CHECK(stale_offer_rejected);

    auto now = ruvia::quic_timestamp{};
    for (std::size_t attempt_value = 0; attempt_value < 8; ++attempt_value) {
        now += std::chrono::milliseconds(1);
        std::array<std::byte, 2048> server_packet{};
        const auto server_result = server.connection(admitted.connection_).write_packet(server_packet, now);
        if (server_result.size_ != 0) {
            const ruvia::quic_datagram_view server_datagram{
                .bytes_ = std::span(server_packet).first(server_result.size_),
                .local_ = server_address,
                .peer_ = client_address};
            (void)client.receive(server_datagram, now);
        }
        std::array<std::byte, 2048> client_packet{};
        const auto client_result = client.write_packet(client_packet, now);
        if (client_result.size_ != 0) {
            const ruvia::quic_datagram_view client_datagram{
                .bytes_ = std::span(client_packet).first(client_result.size_),
                .local_ = client_address,
                .peer_ = server_address};
            (void)server.receive(admitted.connection_, client_datagram, now);
        }
    }
    RUVIA_CHECK(server_tls.completed_);
    RUVIA_CHECK(!server_crypto.random_outputs_.empty());
    if (server_crypto.random_outputs_.empty()) {
        return;
    }
    const ruvia::quic_connection_id server_scid(server_crypto.random_outputs_.front());
    auto route_cid = [&](const ruvia::quic_connection_id& cid) {
        const auto probe_value = cid_probe_packet(cid);
        return server.route_datagram({.bytes_ = probe_value, .local_ = server_address, .peer_ = client_address});
    };
    RUVIA_CHECK_EQ(route_cid(offer.offer_.original_destination_connection_id_).kind_,
        ruvia::quic_server_route_kind::existing_connection);
    RUVIA_CHECK_EQ(route_cid(server_scid).kind_,
        ruvia::quic_server_route_kind::existing_connection);

    std::array<std::byte, 32> short_packet{};
    short_packet.front() = std::byte{0x40};
    std::ranges::copy(server_scid.view(), short_packet.begin() + 1);
    const auto short_route = server.route_datagram({.bytes_ = short_packet,
        .local_ = server_address,
        .peer_ = client_address});
    RUVIA_CHECK_EQ(short_route.kind_, ruvia::quic_server_route_kind::existing_connection);
    RUVIA_CHECK_EQ(short_route.connection_, admitted.connection_);
    short_packet[1] ^= std::byte{0xff};
    RUVIA_CHECK_EQ(server.route_datagram({.bytes_ = short_packet}).kind_,
        ruvia::quic_server_route_kind::dropped);

    // Millisecond configuration must survive ngtcp2's nanosecond duration API.
    now += std::chrono::milliseconds(10);
    (void)server.connection(admitted.connection_).handle_expiry(now);
    RUVIA_CHECK(server.connection(admitted.connection_).info().state_ !=
                ruvia::quic_connection_state::retired);

    bool native_cid_routed{};
    for (std::size_t i = 1; i < server_crypto.random_outputs_.size(); ++i) {
        const auto& bytes_value = server_crypto.random_outputs_[i];
        if (bytes_value.size() < 8 || bytes_value.size() > ruvia::quic_max_connection_id_size) {
            continue;
        }
        const ruvia::quic_connection_id candidate(bytes_value);
        if (candidate == server_scid || candidate == offer.offer_.original_destination_connection_id_) {
            continue;
        }
        const auto routed = route_cid(candidate);
        native_cid_routed |= routed.kind_ == ruvia::quic_server_route_kind::existing_connection &&
                             routed.connection_ == admitted.connection_;
    }
    RUVIA_CHECK(native_cid_routed);

    RUVIA_CHECK_EQ(server.retire(admitted.connection_), ruvia::quic_operation_status::retired);
    RUVIA_CHECK_EQ(server.connection_count(), std::size_t{0});
    RUVIA_CHECK_EQ(server.retire(admitted.connection_), ruvia::quic_operation_status::retired);
    bool stale_token_rejected{};
    try {
        (void)server.connection(admitted.connection_);
    } catch (const std::out_of_range&) {
        stale_token_rejected = true;
    }
    RUVIA_CHECK(stale_token_rejected);
    RUVIA_CHECK_EQ(route_cid(server_scid).kind_, ruvia::quic_server_route_kind::dropped);
    RUVIA_CHECK_EQ(route_cid(offer.offer_.original_destination_connection_id_).kind_,
        ruvia::quic_server_route_kind::dropped);
    for (std::size_t i = 1; i < server_crypto.random_outputs_.size(); ++i) {
        const auto& bytes_value = server_crypto.random_outputs_[i];
        if (bytes_value.size() < 8 || bytes_value.size() > ruvia::quic_max_connection_id_size) {
            continue;
        }
        const ruvia::quic_connection_id candidate(bytes_value);
        RUVIA_CHECK_EQ(route_cid(candidate).kind_, ruvia::quic_server_route_kind::dropped);
    }
}

RUVIA_TEST(quic_server_pending_initial_allocation_failures_leave_no_cached_state) {
    auto packet = initial_packet();
    std::size_t exercised_failures{};
    for (std::size_t fail_after = 0; fail_after < 6; ++fail_after) {
        failing_memory_resource memory;
        ruvia::quic_server server({}, provider(), &memory);
        memory.allocations_before_failure_ = fail_after;
        bool allocation_failed{};
        try {
            (void)server.route_datagram({.bytes_ = packet});
        } catch (const std::bad_alloc&) {
            allocation_failed = true;
        }
        if (allocation_failed) {
            ++exercised_failures;
            RUVIA_CHECK_EQ(server.pending_connection_count(), std::size_t{0});
            memory.allocations_before_failure_ = std::numeric_limits<std::size_t>::max();
            const auto retried = server.route_datagram({.bytes_ = packet});
            RUVIA_CHECK_EQ(retried.kind_, ruvia::quic_server_route_kind::initial_offer);
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
    const auto local = test_address(4433);
    const auto peer = test_address(43001);
    const auto first_offer = server.route_datagram({.bytes_ = std::span(first.bytes_).first(first.size_), .local_ = local, .peer_ = peer});
    const auto first_admitted = server.admit_initial(first_offer.offer_, test_tls_driver(), {});
    RUVIA_CHECK_EQ(first_admitted.status_, ruvia::quic_operation_status::accepted);
    RUVIA_CHECK(!server_crypto.random_outputs_.empty());
    if (server_crypto.random_outputs_.empty()) {
        return;
    }
    const ruvia::quic_connection_id existing_cid(server_crypto.random_outputs_.front());
    auto probe_value = cid_probe_packet(existing_cid);
    const auto existing_route = server.route_datagram({.bytes_ = probe_value, .local_ = local, .peer_ = peer});
    RUVIA_CHECK_EQ(existing_route.kind_, ruvia::quic_server_route_kind::existing_connection);
    RUVIA_CHECK_EQ(existing_route.connection_, first_admitted.connection_);

    deterministic_crypto second_client_crypto;
    auto second = create_client_initial(0x30, second_client_crypto, &memory);
    const auto second_offer = server.route_datagram({.bytes_ = std::span(second.bytes_).first(second.size_), .local_ = local, .peer_ = peer});
    RUVIA_CHECK_EQ(second_offer.kind_, ruvia::quic_server_route_kind::initial_offer);
    server_crypto.forced_random_ = std::vector<std::byte>(
        existing_cid.view().begin(), existing_cid.view().end());
    bool collision_failed{};
    try {
        (void)server.admit_initial(second_offer.offer_, test_tls_driver(), {});
    } catch (const std::runtime_error&) {
        collision_failed = true;
    }
    RUVIA_CHECK(collision_failed);
    RUVIA_CHECK_EQ(server.connection_count(), std::size_t{1});
    RUVIA_CHECK_EQ(server.pending_connection_count(), std::size_t{1});
    const auto after_failure = server.route_datagram({.bytes_ = probe_value, .local_ = local, .peer_ = peer});
    RUVIA_CHECK_EQ(after_failure.kind_, ruvia::quic_server_route_kind::existing_connection);
    RUVIA_CHECK_EQ(after_failure.connection_, first_admitted.connection_);

    server_crypto.forced_random_.reset();
    const auto second_admitted = server.admit_initial(second_offer.offer_, test_tls_driver(), {});
    RUVIA_CHECK_EQ(second_admitted.status_, ruvia::quic_operation_status::accepted);
    RUVIA_CHECK_EQ(server.connection_count(), std::size_t{2});
}

RUVIA_TEST(quic_server_initial_partition_never_falls_back_to_another_worker) {
    std::pmr::monotonic_buffer_resource memory;
    const auto packet = initial_packet();
    constexpr std::uint32_t count = 7;
    const auto partition = ruvia::quic_datagram_partition(packet, count);
    RUVIA_CHECK(partition.has_value());
    if (!partition) {
        return;
    }
    ruvia::quic_server_config wrong_config;
    wrong_config.cid_partition_ = {.index_ = (*partition + 1) % count, .count_ = count};
    ruvia::quic_server wrong_worker(wrong_config, provider(), &memory);
    RUVIA_CHECK_EQ(wrong_worker.route_datagram({.bytes_ = packet}).kind_,
        ruvia::quic_server_route_kind::dropped);
    RUVIA_CHECK_EQ(wrong_worker.pending_connection_count(), std::size_t{0});
    ruvia::quic_server_config full_config;
    full_config.cid_partition_ = {.index_ = *partition, .count_ = count};
    full_config.max_pending_connections_ = 0;
    ruvia::quic_server full_worker(full_config, provider(), &memory);
    RUVIA_CHECK_EQ(full_worker.route_datagram({.bytes_ = packet}).kind_,
        ruvia::quic_server_route_kind::rejected);
    RUVIA_CHECK_EQ(ruvia::quic_datagram_partition(packet, count), partition);
}

RUVIA_TEST(quic_datagram_partition_parses_long_and_short_headers_without_state) {
    auto initial_value = initial_packet();
    constexpr std::uint32_t word = 0x10111213;
    RUVIA_CHECK_EQ(ruvia::quic_datagram_partition(initial_value, 7),
        std::optional<std::uint32_t>(word % 7));
    RUVIA_CHECK_EQ(ruvia::quic_datagram_partition(initial_value, 1),
        std::optional<std::uint32_t>(0));
    RUVIA_CHECK(!ruvia::quic_datagram_partition(initial_value, 0));
    RUVIA_CHECK(!ruvia::quic_datagram_partition({}, 7));
    for (std::size_t size = 0; size < 23; ++size) {
        RUVIA_CHECK(!ruvia::quic_datagram_partition(std::span(initial_value).first(size), 7));
    }
    auto malformed = initial_value;
    malformed[5] = std::byte{21};
    RUVIA_CHECK(!ruvia::quic_datagram_partition(malformed, 7));
    malformed = initial_value;
    malformed[14] = std::byte{21};
    RUVIA_CHECK(!ruvia::quic_datagram_partition(malformed, 7));
    // v2 retains the same version-independent CID layout.
    auto v2 = initial_value;
    v2[0] = std::byte{0xd0};
    v2[1] = std::byte{0x6b};
    v2[2] = std::byte{0x33};
    v2[3] = std::byte{0x43};
    v2[4] = std::byte{0xcf};
    RUVIA_CHECK_EQ(ruvia::quic_datagram_partition(v2, 7),
        ruvia::quic_datagram_partition(initial_value, 7));
    const auto unknown_version = unsupported_version_packet();
    RUVIA_CHECK_EQ(ruvia::quic_datagram_partition(unknown_version, 7),
        ruvia::quic_datagram_partition(initial_value, 7));
    std::array<std::byte, 64> short_packet{};
    short_packet[0] = std::byte{0x40};
    std::ranges::copy(std::span(initial_value).subspan(6, 8), short_packet.begin() + 1);
    RUVIA_CHECK_EQ(ruvia::quic_datagram_partition(short_packet, 7),
        ruvia::quic_datagram_partition(initial_value, 7));
    for (std::size_t size = 0; size < 17; ++size) {
        RUVIA_CHECK(!ruvia::quic_datagram_partition(std::span(short_packet).first(size), 7));
    }
}

RUVIA_TEST(quic_server_rejects_invalid_cid_partitions) {
    std::pmr::monotonic_buffer_resource memory;
    for (const auto partition : {ruvia::quic_cid_partition{.count_ = 0},
             ruvia::quic_cid_partition{.index_ = 3, .count_ = 3},
             ruvia::quic_cid_partition{.index_ = 1, .count_ = 1}}) {
        ruvia::quic_server_config config;
        config.cid_partition_ = partition;
        bool rejected{};
        try {
            ruvia::quic_server server(config, provider(), &memory);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        RUVIA_CHECK(rejected);
    }
}

RUVIA_TEST(quic_server_generated_cids_preserve_partition_through_migration_and_retirement) {
    for (const auto count : {std::uint32_t{1}, std::uint32_t{7},
             std::numeric_limits<std::uint32_t>::max()}) {
        std::pmr::unsynchronized_pool_resource memory;
        deterministic_crypto client_crypto;
        deterministic_crypto server_crypto;
        fake_tls_pair tls_pair;
        fake_tls_endpoint client_tls{.pair_ = &tls_pair, .client_ = true};
        fake_tls_endpoint server_tls{.pair_ = &tls_pair, .client_ = false};
        const auto server_address = test_address(4433);
        const auto client_address = test_address(43001);
        std::array<std::byte, 8> initial_dcid{};
        std::array<std::byte, 8> client_scid{};
        for (std::size_t i = 0; i < initial_dcid.size(); ++i) {
            initial_dcid[i] = static_cast<std::byte>(0x10 + i);
            client_scid[i] = static_cast<std::byte>(0x30 + i);
        }
        ruvia::quic_connection_config client_config;
        client_config.local_address_ = client_address;
        client_config.peer_address_ = server_address;
        client_config.destination_connection_id_ = ruvia::quic_connection_id(initial_dcid);
        client_config.source_connection_id_ = ruvia::quic_connection_id(client_scid);
        ruvia::quic_connection client(client_config, deterministic_provider(client_crypto),
            fake_tls_driver(client_tls), &memory, {});
        std::array<std::byte, 2048> packet{};
        auto now = ruvia::quic_timestamp{};
        const auto initial_value = client.write_packet(packet, now);
        RUVIA_CHECK_EQ(initial_value.status_, ruvia::quic_operation_status::accepted);
        const auto initial_bytes = std::span(packet).first(initial_value.size_);
        const auto partition = ruvia::quic_datagram_partition(initial_bytes, count);
        RUVIA_CHECK(partition.has_value());
        if (!partition) {
            continue;
        }
        ruvia::quic_server_config server_config;
        server_config.cid_partition_ = {.index_ = *partition, .count_ = count};
        server_config.local_transport_parameters_.disable_active_migration_ = false;
        ruvia::quic_server server(server_config, deterministic_provider(server_crypto), &memory);
        const auto offer = server.route_datagram({.bytes_ = initial_bytes,
            .local_ = server_address,
            .peer_ = client_address});
        RUVIA_CHECK_EQ(offer.kind_, ruvia::quic_server_route_kind::initial_offer);
        const auto admitted = server.admit_initial(offer.offer_, fake_tls_driver(server_tls), now);
        RUVIA_CHECK_EQ(admitted.status_, ruvia::quic_operation_status::accepted);
        const auto parameters = server.connection(admitted.connection_).tls_handshake().local_transport_parameters();
        ngtcp2_transport_params decoded_parameters{};
        RUVIA_CHECK_EQ(ngtcp2_transport_params_decode(&decoded_parameters,
                           reinterpret_cast<const std::uint8_t*>(parameters.data()), parameters.size()),
            0);
        const auto& native_scid = decoded_parameters.initial_scid;
        RUVIA_CHECK_EQ(native_scid.datalen, std::size_t{16});
        const ruvia::quic_connection_id server_scid(std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(native_scid.data), native_scid.datalen));
        const auto initial_probe = cid_probe_packet(server_scid);
        RUVIA_CHECK_EQ(ruvia::quic_datagram_partition(initial_probe, count), partition);
        RUVIA_CHECK_EQ(server.route_datagram({.bytes_ = initial_probe}).connection_, admitted.connection_);
        RUVIA_CHECK(!server_crypto.random_outputs_.empty());
        if (!server_crypto.random_outputs_.empty()) {
            RUVIA_CHECK(std::ranges::equal(server_scid.view().subspan(4),
                std::span(server_crypto.random_outputs_.front()).subspan(4)));
            if (count == 1) {
                RUVIA_CHECK(std::ranges::equal(server_scid.view(), server_crypto.random_outputs_.front()));
            }
        }
        std::vector<ruvia::quic_connection_id> observed_cids{server_scid};
        auto pump = [&] {
            now += std::chrono::milliseconds(10);
            if (const auto expiry = server.next_expiry(); expiry && *expiry <= now) {
                (void)server.handle_expiry(now);
            }
            if (const auto expiry = client.next_expiry(); expiry && *expiry <= now) {
                (void)client.handle_expiry(now);
            }
            const auto outbound = server.connection(admitted.connection_).write_packet(packet, now);
            if (outbound.size_ != 0) {
                (void)client.receive({.bytes_ = std::span(packet).first(outbound.size_),
                                         .local_ = outbound.peer_,
                                         .peer_ = outbound.local_},
                    now);
            }
            const auto inbound = client.write_packet(packet, now);
            if (inbound.size_ != 0) {
                const auto bytes_value = std::span(packet).first(inbound.size_);
                RUVIA_CHECK_EQ(ruvia::quic_datagram_partition(bytes_value, count), partition);
                ngtcp2_version_cid ids{};
                RUVIA_CHECK_EQ(ngtcp2_pkt_decode_version_cid(&ids,
                                   reinterpret_cast<const std::uint8_t*>(bytes_value.data()), bytes_value.size(), 16),
                    0);
                const ruvia::quic_connection_id cid(std::span<const std::byte>(
                    reinterpret_cast<const std::byte*>(ids.dcid), ids.dcidlen));
                if (cid.size() == 16 && std::ranges::find(observed_cids, cid) == observed_cids.end()) {
                    observed_cids.push_back(cid);
                }
                const ruvia::quic_datagram_view datagram{.bytes_ = bytes_value,
                    .local_ = inbound.peer_,
                    .peer_ = inbound.local_};
                const auto routed = server.route_datagram(datagram);
                RUVIA_CHECK_EQ(routed.kind_, ruvia::quic_server_route_kind::existing_connection);
                RUVIA_CHECK_EQ(routed.connection_, admitted.connection_);
                (void)server.receive(admitted.connection_, datagram, now);
            }
        };
        for (std::size_t i = 0; i < 16; ++i) {
            pump();
        }
        RUVIA_CHECK(client.info().confirmed_);
        for (const auto port : {std::uint16_t{43002}, std::uint16_t{43003}}) {
            const auto migration = client.start_path_migration(test_address(port));
            RUVIA_CHECK(migration.status_ == ruvia::quic_migration_status::started ||
                        migration.status_ == ruvia::quic_migration_status::validated);
            for (std::size_t i = 0; i < 32; ++i) {
                pump();
            }
            const auto result_value = client.path_migration(migration.id_);
            RUVIA_CHECK(result_value.has_value());
            if (result_value) {
                RUVIA_CHECK_EQ(result_value->status_, ruvia::quic_migration_status::validated);
            }
        }
        // Both migrations consume server-issued NEW_CONNECTION_IDs rather than
        // retaining the handshake CID. Every observed CID selected this worker.
        RUVIA_CHECK(observed_cids.size() >= 3);
        RUVIA_CHECK_EQ(server.retire(admitted.connection_), ruvia::quic_operation_status::retired);
        for (const auto& cid : observed_cids) {
            const auto probe_value = cid_probe_packet(cid);
            RUVIA_CHECK_EQ(ruvia::quic_datagram_partition(probe_value, count), partition);
            RUVIA_CHECK_EQ(server.route_datagram({.bytes_ = probe_value}).kind_,
                ruvia::quic_server_route_kind::dropped);
        }
    }
}

}  // namespace
