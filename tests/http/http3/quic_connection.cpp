#include "ruvia/http/quic_connection.h"

#include <ngtcp2/ngtcp2.h>

#include <algorithm>
#include <array>
#include <limits>
#include <memory_resource>
#include <new>
#include <stdexcept>

#include "ruvia/http/quic_server.h"

#include "test_harness.h"

namespace {

class counting_resource final : public std::pmr::memory_resource {
public:
    std::size_t allocations_{};
    std::size_t deallocations_{};

private:
    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override {
        ++allocations_;
        return std::pmr::new_delete_resource()->allocate(bytes_value, alignment);
    }
    void do_deallocate(void* pointer, std::size_t bytes_value, std::size_t alignment) override {
        ++deallocations_;
        std::pmr::new_delete_resource()->deallocate(pointer, bytes_value, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

struct primitive {};
void destroy_primitive(void* opaque) noexcept {
    delete static_cast<primitive*>(opaque);
}
void seal_primitive(void*, std::span<const std::byte, 12>, std::span<const std::byte>,
    std::span<const std::byte>, std::span<std::byte> output) {
    std::ranges::fill(output, std::byte{0});
}
ruvia::quic_aead_key_operations::open_result open_primitive(void*,
    std::span<const std::byte, 12>, std::span<const std::byte>,
    std::span<const std::byte>, std::span<std::byte>) {
    return {.value_ = ruvia::quic_aead_key_operations::open_result::status::rejected};
}
void mask_primitive(void*, std::span<const std::byte, 16>, std::span<std::byte, 5> output) {
    std::ranges::fill(output, std::byte{0});
}
void random_bytes(void*, std::span<std::byte> output) {
    std::ranges::fill(output, std::byte{0x39});
}
void hkdf(void*, ruvia::quic_cipher_suite, std::span<const std::byte>,
    std::span<const std::byte>, std::span<std::byte> output) {
    std::ranges::fill(output, std::byte{0x45});
}
ruvia::quic_aead_key make_aead(void*, ruvia::quic_cipher_suite,
    ruvia::quic_crypto_direction, std::span<const std::byte>) {
    return ruvia::quic_aead_key::adopt(new primitive{},
        {.destroy_ = destroy_primitive, .seal_ = seal_primitive, .open_ = open_primitive});
}
ruvia::quic_aead_key fail_aead_factory(void*, ruvia::quic_cipher_suite,
    ruvia::quic_crypto_direction, std::span<const std::byte>) {
    throw std::runtime_error("injected Initial AEAD factory failure");
}
ruvia::quic_header_protection_key make_header(void*, ruvia::quic_cipher_suite,
    std::span<const std::byte>) {
    return ruvia::quic_header_protection_key::adopt(new primitive{},
        {.destroy_ = destroy_primitive, .mask_ = mask_primitive});
}
void secure_erase(void*, std::span<std::byte> bytes_value) noexcept {
    std::ranges::fill(bytes_value, std::byte{0});
}
ruvia::quic_tls_drive_result drive_tls(void*, ruvia::quic_tls_handshake&) noexcept {
    return {};
}
void retire_tls(void*) noexcept {}
struct retire_context {
    std::size_t calls_{};
};
void count_retire(void* opaque) noexcept {
    ++static_cast<retire_context*>(opaque)->calls_;
}

ruvia::quic_crypto_provider_view provider() {
    return {.random_bytes_ = random_bytes,
        .hkdf_extract_ = hkdf,
        .hkdf_expand_ = hkdf,
        .create_aead_key_ = make_aead,
        .create_header_protection_key_ = make_header,
        .secure_erase_ = secure_erase};
}

ruvia::quic_connection_config client_config() {
    ruvia::quic_connection_config config;
    config.local_address_.port_ = 41000;
    config.peer_address_.port_ = 4433;
    const std::array dcid{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4},
        std::byte{5}, std::byte{6}, std::byte{7}, std::byte{8}};
    const std::array scid{std::byte{9}, std::byte{10}, std::byte{11}, std::byte{12}};
    config.destination_connection_id_ = ruvia::quic_connection_id(dcid);
    config.source_connection_id_ = ruvia::quic_connection_id(scid);
    return config;
}

ngtcp2_transport_params decode_local_transport_parameters(
    ruvia::quic_connection& connection) {
    const auto encoded = connection.tls_handshake().local_transport_parameters();
    ngtcp2_transport_params native{};
    const int result_value = ngtcp2_transport_params_decode(&native,
        reinterpret_cast<const std::uint8_t*>(encoded.data()), encoded.size());
    if (result_value != 0) {
        throw std::runtime_error("failed to decode facade local transport parameters");
    }
    return native;
}

}  // namespace

RUVIA_TEST(quic_connection_native_memory_uses_and_releases_the_owner_resource) {
    counting_resource resource;
    {
        ruvia::quic_connection connection(client_config(), provider(),
            {.drive_ = drive_tls, .retire_ = retire_tls}, &resource, ruvia::quic_timestamp{} + std::chrono::seconds(1));
        RUVIA_CHECK_EQ(connection.info().state_, ruvia::quic_connection_state::connecting);
        RUVIA_CHECK(connection.tls_handshake().role() == ruvia::quic_role::client);
        RUVIA_CHECK(resource.allocations_ > 0);
        RUVIA_CHECK(resource.deallocations_ < resource.allocations_);
    }
    RUVIA_CHECK_EQ(resource.allocations_, resource.deallocations_);
}

RUVIA_TEST(quic_connection_closing_keeps_tls_driver_and_packet_keys_until_retirement) {
    counting_resource resource;
    retire_context retire_state;
    {
        ruvia::quic_connection connection(client_config(), provider(),
            {.context_ = &retire_state, .drive_ = drive_tls, .retire_ = count_retire},
            &resource, ruvia::quic_timestamp{} + std::chrono::seconds(1));
        RUVIA_CHECK(connection.close({.kind_ = ruvia::quic_close_kind::application,
                        .code_ = 0x31,
                        .frame_type_ = 0,
                        .reason_ = "shutdown"}) == ruvia::quic_operation_status::accepted);
        std::array<std::byte, 1400> packet{};
        const auto result_value = connection.write_packet(packet,
            ruvia::quic_timestamp{} + std::chrono::seconds(2));
        RUVIA_CHECK(result_value.status_ == ruvia::quic_operation_status::closing);
        RUVIA_CHECK_EQ(retire_state.calls_, std::size_t{0});
        RUVIA_CHECK(resource.deallocations_ < resource.allocations_);
        // The closing period ends at its reported expiry, which retires the connection.
        const auto expiry = connection.next_expiry();
        RUVIA_CHECK(expiry.has_value());
        if (expiry) {
            RUVIA_CHECK(*expiry > ruvia::quic_timestamp{} + std::chrono::seconds(2));
            RUVIA_CHECK_EQ(connection.handle_expiry(*expiry - std::chrono::nanoseconds(1)),
                ruvia::quic_operation_status::closing);
            RUVIA_CHECK_EQ(retire_state.calls_, std::size_t{0});
            RUVIA_CHECK_EQ(connection.handle_expiry(*expiry), ruvia::quic_operation_status::retired);
            RUVIA_CHECK_EQ(connection.info().state_, ruvia::quic_connection_state::retired);
            RUVIA_CHECK_EQ(retire_state.calls_, std::size_t{1});
        }
    }
    RUVIA_CHECK_EQ(retire_state.calls_, std::size_t{1});
    RUVIA_CHECK_EQ(resource.allocations_, resource.deallocations_);
}

RUVIA_TEST(quic_connection_close_enforces_error_code_range_without_changing_state) {
    for (const auto kind : {ruvia::quic_close_kind::transport,
             ruvia::quic_close_kind::application, ruvia::quic_close_kind::tls}) {
        counting_resource resource;
        {
            ruvia::quic_connection connection(client_config(), provider(),
                {.drive_ = drive_tls, .retire_ = retire_tls}, &resource,
                ruvia::quic_timestamp{} + std::chrono::seconds(1));
            for (const auto code : {std::uint64_t{1} << 62, std::numeric_limits<std::uint64_t>::max()}) {
                bool rejected{};
                try {
                    (void)connection.close({.kind_ = kind, .code_ = code});
                } catch (const ruvia::quic_error& error) {
                    rejected = error.code() == ruvia::quic_error_code::invalid_configuration;
                }
                RUVIA_CHECK(rejected);
                RUVIA_CHECK_EQ(connection.info().state_, ruvia::quic_connection_state::connecting);
                RUVIA_CHECK_EQ(connection.info().close_error_code_, std::uint64_t{0});
            }
            constexpr auto maximum = (std::uint64_t{1} << 62) - 1;
            RUVIA_CHECK_EQ(connection.close({.kind_ = kind, .code_ = maximum}), ruvia::quic_operation_status::accepted);
            RUVIA_CHECK_EQ(connection.info().state_, ruvia::quic_connection_state::closing);
            RUVIA_CHECK_EQ(connection.info().close_error_code_, maximum);
            RUVIA_CHECK_EQ(connection.close({.kind_ = kind, .code_ = 0}), ruvia::quic_operation_status::accepted);
            RUVIA_CHECK_EQ(connection.info().close_error_code_, maximum);
        }
        RUVIA_CHECK_EQ(resource.allocations_, resource.deallocations_);
    }
}

RUVIA_TEST(quic_connection_requires_tls_driver_retirement_callback) {
    counting_resource resource;
    retire_context retire_state;
    bool rejected{};
    try {
        ruvia::quic_connection connection(client_config(), provider(),
            {.context_ = &retire_state, .drive_ = drive_tls}, &resource,
            ruvia::quic_timestamp{} + std::chrono::seconds(1));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
    RUVIA_CHECK_EQ(retire_state.calls_, std::size_t{0});
    RUVIA_CHECK_EQ(resource.allocations_, resource.deallocations_);
}

RUVIA_TEST(quic_connection_rejects_missing_resource) {
    bool rejected{};
    try {
        ruvia::quic_connection connection(client_config(), provider(),
            {.drive_ = drive_tls, .retire_ = retire_tls}, nullptr,
            ruvia::quic_timestamp{} + std::chrono::seconds(1));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
}

RUVIA_TEST(quic_connection_public_configuration_supports_both_roles_and_zero_peer_scid) {
    counting_resource resource;
    auto config = client_config();
    config.role_ = ruvia::quic_role::server;
    config.destination_connection_id_ = {};
    config.source_connection_id_.reset();
    const std::array original_dcid{std::byte{0x10}, std::byte{0x11}, std::byte{0x12}, std::byte{0x13},
        std::byte{0x14}, std::byte{0x15}, std::byte{0x16}, std::byte{0x17}};
    config.original_destination_connection_id_ = ruvia::quic_connection_id(original_dcid);
    {
        ruvia::quic_connection connection(config, provider(), {.drive_ = drive_tls, .retire_ = retire_tls}, &resource,
            ruvia::quic_timestamp{} + std::chrono::seconds(1));
        RUVIA_CHECK(connection.tls_handshake().role() == ruvia::quic_role::server);
    }
    RUVIA_CHECK_EQ(resource.allocations_, resource.deallocations_);
}

RUVIA_TEST(quic_connection_preserves_explicit_empty_scid_and_defaults_nullopt_to_random_sixteen) {
    counting_resource resource;
    {
        auto config = client_config();
        config.source_connection_id_ = ruvia::quic_connection_id{};
        ruvia::quic_connection connection(config, provider(), {.drive_ = drive_tls, .retire_ = retire_tls}, &resource,
            ruvia::quic_timestamp{} + std::chrono::seconds(1));
        const auto params = decode_local_transport_parameters(connection);
        RUVIA_CHECK(params.initial_scid_present != 0);
        RUVIA_CHECK_EQ(params.initial_scid.datalen, std::size_t{0});
    }
    {
        auto config = client_config();
        config.source_connection_id_.reset();
        ruvia::quic_connection connection(config, provider(), {.drive_ = drive_tls, .retire_ = retire_tls}, &resource,
            ruvia::quic_timestamp{} + std::chrono::seconds(1));
        const auto params = decode_local_transport_parameters(connection);
        RUVIA_CHECK(params.initial_scid_present != 0);
        RUVIA_CHECK_EQ(params.initial_scid.datalen, std::size_t{16});
    }
    RUVIA_CHECK_EQ(resource.allocations_, resource.deallocations_);
}

RUVIA_TEST(quic_connection_generated_server_source_cid_matches_its_partition) {
    counting_resource resource;
    for (const auto partition : {ruvia::quic_cid_partition{.index_ = 5, .count_ = 7},
             ruvia::quic_cid_partition{.index_ = std::numeric_limits<std::uint32_t>::max() - 1,
                 .count_ = std::numeric_limits<std::uint32_t>::max()}}) {
        auto config = client_config();
        config.role_ = ruvia::quic_role::server;
        config.original_destination_connection_id_ = config.destination_connection_id_;
        config.source_connection_id_.reset();
        config.cid_partition_ = partition;
        ruvia::quic_connection connection(config, provider(),
            {.drive_ = drive_tls, .retire_ = retire_tls}, &resource,
            ruvia::quic_timestamp{} + std::chrono::seconds(1));
        const auto params = decode_local_transport_parameters(connection);
        RUVIA_CHECK_EQ(params.initial_scid.datalen, std::size_t{16});
        std::array<std::byte, 32> packet{};
        packet[0] = std::byte{0x40};
        std::ranges::copy(std::span<const std::byte>(
                              reinterpret_cast<const std::byte*>(params.initial_scid.data), params.initial_scid.datalen),
            packet.begin() + 1);
        RUVIA_CHECK(ruvia::quic_datagram_partition(packet, partition.count_) == partition.index_);
    }
    RUVIA_CHECK_EQ(resource.allocations_, resource.deallocations_);
}

RUVIA_TEST(quic_connection_native_initial_key_factory_rollback_retires_bound_driver) {
    counting_resource resource;
    retire_context retire_state;
    auto crypto = provider();
    crypto.create_aead_key_ = fail_aead_factory;
    bool rejected{};
    try {
        ruvia::quic_connection connection(client_config(), crypto,
            {.context_ = &retire_state, .drive_ = drive_tls, .retire_ = count_retire},
            &resource, ruvia::quic_timestamp{} + std::chrono::seconds(1));
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
    RUVIA_CHECK_EQ(retire_state.calls_, std::size_t{1});
    RUVIA_CHECK_EQ(resource.allocations_, resource.deallocations_);
}

RUVIA_TEST(quic_connection_rejects_missing_server_original_dcid_and_malformed_local_limits) {
    auto server = client_config();
    server.role_ = ruvia::quic_role::server;
    server.original_destination_connection_id_.reset();
    bool missing_original_rejected{};
    try {
        ruvia::quic_connection connection(server, provider(), {.drive_ = drive_tls, .retire_ = retire_tls},
            std::pmr::new_delete_resource(),
            ruvia::quic_timestamp{} + std::chrono::seconds(1));
    } catch (const std::invalid_argument&) {
        missing_original_rejected = true;
    }
    RUVIA_CHECK(missing_original_rejected);

    const auto rejects = [](ruvia::quic_connection_config invalid) {
        try {
            ruvia::quic_connection connection(std::move(invalid), provider(),
                {.drive_ = drive_tls, .retire_ = retire_tls}, std::pmr::new_delete_resource(),
                ruvia::quic_timestamp{} + std::chrono::seconds(1));
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };
    auto invalid = client_config();
    invalid.local_transport_parameters_.max_udp_payload_size_ = 1199;
    RUVIA_CHECK(rejects(invalid));
    invalid = client_config();
    invalid.limits_.max_datagram_size_ = 1199;
    RUVIA_CHECK(rejects(invalid));
    invalid = client_config();
    invalid.local_transport_parameters_.active_connection_id_limit_ = 1;
    RUVIA_CHECK(rejects(invalid));
    invalid = client_config();
    invalid.local_transport_parameters_.initial_max_data_ = std::uint64_t{1} << 62;
    RUVIA_CHECK(rejects(invalid));
    invalid = client_config();
    invalid.local_transport_parameters_.initial_max_streams_bidi_ = std::uint64_t{1} << 60;
    RUVIA_CHECK(rejects(invalid));
    invalid = client_config();
    invalid.local_transport_parameters_.idle_timeout_ms_ =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) / 1'000'000 + 1;
    RUVIA_CHECK(rejects(invalid));
    invalid = client_config();
    invalid.cid_partition_.count_ = 0;
    RUVIA_CHECK(rejects(invalid));
    invalid = client_config();
    invalid.cid_partition_ = {.index_ = 3, .count_ = 3};
    RUVIA_CHECK(rejects(invalid));
    invalid = client_config();
    invalid.role_ = ruvia::quic_role::server;
    invalid.original_destination_connection_id_ = invalid.destination_connection_id_;
    invalid.cid_partition_ = {.index_ = 1, .count_ = 2};
    RUVIA_CHECK(rejects(invalid));
    invalid.source_connection_id_ = ruvia::quic_connection_id(std::array<std::byte, 16>{});
    RUVIA_CHECK(rejects(invalid));
}
