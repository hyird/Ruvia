#include "http3/quic_stream.h"

#include <array>
#include <limits>
#include <memory_resource>
#include <new>
#include <stdexcept>

#include "test_harness.h"

namespace {

class counting_resource final : public std::pmr::memory_resource {
public:
    std::size_t live_bytes{};
    std::size_t live_blocks{};
    std::size_t allocations_before_failure{std::numeric_limits<std::size_t>::max()};

private:
    void* do_allocate(std::size_t size, std::size_t alignment) override {
        if (allocations_before_failure == 0) {
            allocations_before_failure = std::numeric_limits<std::size_t>::max();
            throw std::bad_alloc();
        }
        if (allocations_before_failure != std::numeric_limits<std::size_t>::max()) {
            --allocations_before_failure;
        }
        void* const result = std::pmr::new_delete_resource()->allocate(size, alignment);
        live_bytes += size;
        ++live_blocks;
        return result;
    }

    void do_deallocate(void* pointer, std::size_t size, std::size_t alignment) override {
        live_bytes -= size;
        --live_blocks;
        std::pmr::new_delete_resource()->deallocate(pointer, size, alignment);
    }

    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

void random_bytes(void*, std::span<std::byte>) {}
void hkdf_extract(void*, ruvia::quic_cipher_suite, std::span<const std::byte>,
    std::span<const std::byte>, std::span<std::byte>) {}
void hkdf_expand(void*, ruvia::quic_cipher_suite, std::span<const std::byte>,
    std::span<const std::byte>, std::span<std::byte>) {}
ruvia::quic_aead_key create_aead(void*, ruvia::quic_cipher_suite,
    ruvia::quic_crypto_direction, std::span<const std::byte>) {
    return {};
}
ruvia::quic_header_protection_key create_header(void*, ruvia::quic_cipher_suite,
    std::span<const std::byte>) {
    return {};
}
void secure_erase(void*, std::span<std::byte>) noexcept {}
ruvia::quic_tls_drive_result drive_tls(void*, ruvia::quic_tls_handshake&) noexcept {
    return {};
}
void retire_tls(void*) noexcept {}

ruvia::quic_crypto_provider_view provider() {
    return {.random_bytes = random_bytes,
        .hkdf_extract = hkdf_extract,
        .hkdf_expand = hkdf_expand,
        .create_aead_key = create_aead,
        .create_header_protection_key = create_header,
        .secure_erase = secure_erase};
}

ruvia::quic_connection_config config() {
    ruvia::quic_connection_config result;
    result.local_address.port = 443;
    result.peer_address.port = 50000;
    const std::array dcid{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4},
        std::byte{5}, std::byte{6}, std::byte{7}, std::byte{8}};
    const std::array scid{std::byte{8}, std::byte{7}, std::byte{6}, std::byte{5},
        std::byte{4}, std::byte{3}, std::byte{2}, std::byte{1}};
    result.destination_connection_id = ruvia::quic_connection_id(dcid);
    result.source_connection_id = ruvia::quic_connection_id(scid);
    result.limits.max_stream_buffer_size = 8;
    result.limits.max_connection_buffer_size = 32;
    result.local_transport_parameters.initial_max_data = 32;
    result.local_transport_parameters.initial_max_stream_data_bidi_local = 8;
    result.local_transport_parameters.initial_max_stream_data_bidi_remote = 8;
    result.local_transport_parameters.initial_max_stream_data_uni = 8;
    result.local_transport_parameters.initial_max_streams_bidi = 2;
    result.local_transport_parameters.initial_max_streams_uni = 2;
    result.limits.max_datagram_size = 1200;
    result.limits.max_datagrams = 2;
    return result;
}

std::span<const std::byte> bytes(const char* value, std::size_t size) {
    return {reinterpret_cast<const std::byte*>(value), size};
}

std::uint64_t open_peer_bidi(ruvia::detail::quic_connection_state& state,
    ruvia::testing::TestContext& ruvia_ctx) {
    constexpr std::uint64_t stream_id = 1;
    RUVIA_CHECK_EQ(ruvia::detail::quic_stream_open_callback(nullptr,
                       static_cast<std::int64_t>(stream_id), &state),
        0);
    return stream_id;
}

void test_receive_copy_consume_and_append(ruvia::testing::TestContext& ruvia_ctx) {
    ngtcp2_callbacks callbacks{};
    ruvia::detail::configure_quic_stream_callbacks(callbacks);
    RUVIA_CHECK(callbacks.stream_open == &ruvia::detail::quic_stream_open_callback);
    RUVIA_CHECK(callbacks.recv_stream_data == &ruvia::detail::quic_stream_data_callback);
    RUVIA_CHECK(callbacks.stream_reset == &ruvia::detail::quic_stream_reset_callback);
    RUVIA_CHECK(callbacks.recv_stop_sending == &ruvia::detail::quic_stream_stop_sending_callback);
    RUVIA_CHECK(callbacks.stream_close2 == &ruvia::detail::quic_stream_close_callback);
    RUVIA_CHECK(callbacks.acked_stream_data_offset == &ruvia::detail::quic_stream_acknowledged_callback);
    RUVIA_CHECK(callbacks.recv_datagram == &ruvia::detail::quic_datagram_received_callback);
    RUVIA_CHECK(callbacks.ack_datagram == &ruvia::detail::quic_datagram_acknowledged_callback);
    RUVIA_CHECK(callbacks.lost_datagram == &ruvia::detail::quic_datagram_lost_callback);
    counting_resource resource;
    {
        ruvia::detail::quic_connection_state state(config(), provider(),
            {.drive = drive_tls, .retire = retire_tls}, &resource, {});
        const auto stream_id = open_peer_bidi(state, ruvia_ctx);
        RUVIA_CHECK_EQ(ruvia::detail::quic_stream_data_callback(nullptr, 0,
                           static_cast<std::int64_t>(stream_id), 0,
                           reinterpret_cast<const std::uint8_t*>("abcd"), 4, &state, nullptr),
            0);

        std::array<std::byte, 2> first{};
        const auto first_read = ruvia::detail::read_stream_buffer(state, stream_id, first);
        RUVIA_CHECK(first_read.status == ruvia::quic_stream_read_status::data);
        RUVIA_CHECK_EQ(first_read.size, std::size_t{2});
        RUVIA_CHECK_EQ(first[0], std::byte{'a'});
        RUVIA_CHECK_EQ(first[1], std::byte{'b'});
        RUVIA_CHECK_EQ(state.retained_stream_input_bytes_, std::size_t{2});

        RUVIA_CHECK_EQ(ruvia::detail::quic_stream_data_callback(nullptr, 0,
                           static_cast<std::int64_t>(stream_id), 4,
                           reinterpret_cast<const std::uint8_t*>("efgh"), 4, &state, nullptr),
            0);
        std::array<std::byte, 8> rest{};
        const auto rest_read = ruvia::detail::read_stream_buffer(state, stream_id, rest);
        RUVIA_CHECK_EQ(rest_read.size, std::size_t{6});
        RUVIA_CHECK_EQ(rest[0], std::byte{'c'});
        RUVIA_CHECK_EQ(rest[1], std::byte{'d'});
        RUVIA_CHECK_EQ(rest[2], std::byte{'e'});
        RUVIA_CHECK_EQ(rest[5], std::byte{'h'});
        RUVIA_CHECK_EQ(state.retained_stream_input_bytes_, std::size_t{0});

        RUVIA_CHECK_EQ(ruvia::detail::quic_stream_data_callback(nullptr,
                           NGTCP2_STREAM_DATA_FLAG_FIN, static_cast<std::int64_t>(stream_id),
                           8, nullptr, 0, &state, nullptr),
            0);
        const auto eof = ruvia::detail::read_stream_buffer(state, stream_id, rest);
        RUVIA_CHECK(eof.status == ruvia::quic_stream_read_status::fin);
    }
    RUVIA_CHECK_EQ(resource.live_blocks, std::size_t{0});
    RUVIA_CHECK_EQ(resource.live_bytes, std::size_t{0});
}

void test_send_ranges_ack_release_and_fair_selection(ruvia::testing::TestContext& ruvia_ctx) {
    counting_resource resource;
    {
        auto test_config = config();
        test_config.limits.max_stream_buffer_size = 64;
        test_config.limits.max_connection_buffer_size = 128;
        test_config.local_transport_parameters.initial_max_data = 128;
        test_config.local_transport_parameters.initial_max_stream_data_bidi_local = 64;
        test_config.local_transport_parameters.initial_max_stream_data_bidi_remote = 64;
        test_config.local_transport_parameters.initial_max_stream_data_uni = 64;
        ruvia::detail::quic_connection_state state(std::move(test_config), provider(),
            {.drive = drive_tls, .retire = retire_tls}, &resource, {});
        state.streams_.try_emplace(0, &resource);
        auto& first = state.streams_.at(0);
        first.readable = true;
        first.writable = true;
        state.streams_.try_emplace(4, &resource);
        auto& second = state.streams_.at(4);
        second.readable = true;
        second.writable = true;

        constexpr char first_bytes[] = "abcdefghijklmnopqrstuvwxyzABCDEF";
        constexpr char second_bytes[] = "01234567890123456789012345678901";
        constexpr char repeated_bytes[] = "ZYXWVUTSRQPONMLKJIHGFEDCBA987654";
        const auto baseline_bytes = resource.live_bytes;
        auto first_write = ruvia::detail::queue_stream_write(state, 0, bytes(first_bytes, 32), false);
        RUVIA_CHECK_EQ(first_write.accepted, std::size_t{32});
        RUVIA_CHECK_EQ(ruvia::detail::queue_stream_write(state, 0, bytes(second_bytes, 32), false).accepted,
            std::size_t{32});
        RUVIA_CHECK_EQ(ruvia::detail::queue_stream_write(state, 4, bytes(second_bytes, 32), false).accepted,
            std::size_t{32});
        const auto retained_bytes = resource.live_bytes;
        RUVIA_CHECK(retained_bytes > baseline_bytes);

        auto selected = ruvia::detail::next_stream_write(state);
        RUVIA_CHECK_EQ(selected.stream_id, std::uint64_t{0});
        RUVIA_CHECK_EQ(selected.range_count, std::size_t{2});
        const auto second_block_pointer = selected.ranges[1].data();
        ruvia::detail::commit_stream_write(state, selected, 64, false);
        auto fair = ruvia::detail::next_stream_write(state);
        RUVIA_CHECK_EQ(fair.stream_id, std::uint64_t{4});
        ruvia::detail::commit_stream_write(state, fair, 32, false);

        RUVIA_CHECK_EQ(ruvia::detail::quic_stream_acknowledged_callback(nullptr, 0, 0, 16,
                           &state, nullptr),
            0);
        RUVIA_CHECK_EQ(state.retained_stream_output_bytes_, std::size_t{96});
        RUVIA_CHECK_EQ(ruvia::detail::quic_stream_acknowledged_callback(nullptr, 0, 16, 16,
                           &state, nullptr),
            0);
        RUVIA_CHECK_EQ(state.retained_stream_output_bytes_, std::size_t{64});
        const auto& retained_block = state.streams_.at(0).output.front();
        RUVIA_CHECK_EQ(reinterpret_cast<const std::byte*>(retained_block.bytes.data()),
            second_block_pointer);
        RUVIA_CHECK_EQ(retained_block.bytes.size(), std::size_t{32});
        RUVIA_CHECK_EQ(ruvia::detail::quic_stream_acknowledged_callback(nullptr, 0, 32, 32,
                           &state, nullptr),
            0);
        RUVIA_CHECK_EQ(state.retained_stream_output_bytes_, std::size_t{32});
        RUVIA_CHECK_EQ(ruvia::detail::quic_stream_acknowledged_callback(nullptr, 4, 0, 32,
                           &state, nullptr),
            0);
        RUVIA_CHECK_EQ(state.retained_stream_output_bytes_, std::size_t{0});
        RUVIA_CHECK(resource.live_bytes < retained_bytes);
        RUVIA_CHECK_EQ(ruvia::detail::quic_stream_close_callback(nullptr, 0, 0, 0, 0,
                           &state, nullptr),
            0);
        RUVIA_CHECK_EQ(state.retained_stream_output_bytes_, std::size_t{0});

        RUVIA_CHECK(resource.live_bytes < retained_bytes);
        std::size_t plateau_bytes{};
        for (std::size_t iteration = 0; iteration < 128; ++iteration) {
            const auto offset = state.streams_.at(4).send_offset;
            const auto repeated = ruvia::detail::queue_stream_write(
                state, 4, bytes(repeated_bytes, 32), false);
            RUVIA_CHECK_EQ(repeated.accepted, std::size_t{32});
            const auto repeated_write = ruvia::detail::next_stream_write(state);
            RUVIA_CHECK_EQ(repeated_write.stream_id, std::uint64_t{4});
            ruvia::detail::commit_stream_write(state, repeated_write, 32, false);
            RUVIA_CHECK_EQ(ruvia::detail::quic_stream_acknowledged_callback(
                               nullptr, 4, offset, 12, &state, nullptr),
                0);
            RUVIA_CHECK_EQ(state.retained_stream_output_bytes_, std::size_t{32});
            RUVIA_CHECK_EQ(ruvia::detail::quic_stream_acknowledged_callback(
                               nullptr, 4, offset + 12, 20, &state, nullptr),
                0);
            RUVIA_CHECK_EQ(state.retained_stream_output_bytes_, std::size_t{0});
            if (iteration == 0) {
                plateau_bytes = resource.live_bytes;
            } else {
                RUVIA_CHECK_EQ(resource.live_bytes, plateau_bytes);
            }
        }

        state.streams_.try_emplace(8, &resource);
        auto& empty_fin_stream = state.streams_.at(8);
        empty_fin_stream.writable = true;
        RUVIA_CHECK(ruvia::detail::queue_stream_write(state, 8, {}, true).status ==
                    ruvia::quic_operation_status::accepted);
        const auto empty_fin = ruvia::detail::next_stream_write(state);
        RUVIA_CHECK_EQ(empty_fin.stream_id, std::uint64_t{8});
        RUVIA_CHECK(empty_fin.fin);
        ruvia::detail::commit_stream_write(state, empty_fin, 0, true);
        const auto other_offset = state.streams_.at(4).send_offset;
        RUVIA_CHECK_EQ(ruvia::detail::queue_stream_write(
                           state, 4, bytes(second_bytes, 32), false)
                           .accepted,
            std::size_t{32});
        const auto other_write = ruvia::detail::next_stream_write(state);
        RUVIA_CHECK_EQ(other_write.stream_id, std::uint64_t{4});
        ruvia::detail::commit_stream_write(state, other_write, 32, false);
        RUVIA_CHECK_EQ(ruvia::detail::quic_stream_acknowledged_callback(
                           nullptr, 8, 0, 0, &state, nullptr),
            0);
        RUVIA_CHECK_EQ(state.retained_stream_output_bytes_, std::size_t{32});
        RUVIA_CHECK_EQ(ruvia::detail::quic_stream_acknowledged_callback(
                           nullptr, 4, other_offset, 32, &state, nullptr),
            0);
        RUVIA_CHECK_EQ(state.retained_stream_output_bytes_, std::size_t{0});
    }
    RUVIA_CHECK_EQ(resource.live_blocks, std::size_t{0});
    RUVIA_CHECK_EQ(resource.live_bytes, std::size_t{0});
}

void test_stream_and_datagram_budgets_empty_values_and_cold_release(
    ruvia::testing::TestContext& ruvia_ctx) {
    counting_resource resource;
    {
        ruvia::detail::quic_connection_state state(config(), provider(),
            {.drive = drive_tls, .retire = retire_tls}, &resource, {});
        const auto stream_id = open_peer_bidi(state, ruvia_ctx);
        RUVIA_CHECK_EQ(ruvia::detail::queue_stream_write(state, stream_id,
                           bytes("12345678", 8), false)
                           .accepted,
            std::size_t{8});
        const auto blocked = ruvia::detail::queue_stream_write(state, stream_id,
            bytes("x", 1), false);
        RUVIA_CHECK(blocked.status == ruvia::quic_operation_status::would_block);
        RUVIA_CHECK_EQ(state.retained_stream_output_bytes_, std::size_t{8});

        RUVIA_CHECK(ruvia::detail::queue_datagram_with_limit(state, {}, 12) ==
                    ruvia::quic_datagram_write_status::queued);
        RUVIA_CHECK(ruvia::detail::queue_datagram_with_limit(state, bytes("abcd", 4), 12) ==
                    ruvia::quic_datagram_write_status::queued);
        RUVIA_CHECK(ruvia::detail::queue_datagram_with_limit(state, bytes("toolarge", 8), 4) ==
                    ruvia::quic_datagram_write_status::too_large);
        RUVIA_CHECK(ruvia::detail::queue_datagram_with_limit(state, bytes("w", 1), 12) ==
                    ruvia::quic_datagram_write_status::dropped);
        auto empty = ruvia::detail::next_datagram_write_with_limit(state, 12);
        RUVIA_CHECK(empty.available);
        RUVIA_CHECK_EQ(empty.id, std::uint64_t{0});
        RUVIA_CHECK(empty.bytes.empty());
        ruvia::detail::commit_datagram_write(state, empty.id, true);
        RUVIA_CHECK(ruvia::detail::queue_datagram_with_limit(state, bytes("z", 1), 12) ==
                    ruvia::quic_datagram_write_status::queued);
        auto next = ruvia::detail::next_datagram_write_with_limit(state, 2);
        RUVIA_CHECK(next.available);
        RUVIA_CHECK_EQ(next.bytes.size(), std::size_t{1});
        RUVIA_CHECK_EQ(next.bytes[0], std::byte{'z'});
        ruvia::detail::commit_datagram_write(state, next.id, false);
        RUVIA_CHECK_EQ(state.send_datagram_bytes_, std::size_t{1});
        next = ruvia::detail::next_datagram_write_with_limit(state, 12);
        RUVIA_CHECK(next.available);
        RUVIA_CHECK_EQ(next.bytes.size(), std::size_t{1});
        ruvia::detail::commit_datagram_write(state, next.id, true);
        RUVIA_CHECK_EQ(state.send_datagram_bytes_, std::size_t{0});

        RUVIA_CHECK_EQ(ruvia::detail::quic_datagram_received_callback(nullptr, 0,
                           nullptr, 0, &state),
            0);
        std::array<std::byte, 4> output{};
        const auto empty_received = ruvia::detail::read_datagram_buffer(state, output);
        RUVIA_CHECK(empty_received.status == ruvia::quic_datagram_status::received);
        RUVIA_CHECK_EQ(empty_received.size, std::size_t{0});
        RUVIA_CHECK(ruvia::detail::read_datagram_buffer(state, output).status ==
                    ruvia::quic_datagram_status::would_block);
        const auto before_receive = resource.live_bytes;
        RUVIA_CHECK_EQ(ruvia::detail::quic_datagram_received_callback(nullptr, 0,
                           reinterpret_cast<const std::uint8_t*>("msg"), 3, &state),
            0);
        std::array<std::byte, 2> too_small{};
        RUVIA_CHECK(ruvia::detail::read_datagram_buffer(state, too_small).status ==
                    ruvia::quic_datagram_status::too_large);
        RUVIA_CHECK_EQ(state.received_datagram_bytes_, std::size_t{3});
        const auto received = ruvia::detail::read_datagram_buffer(state, output);
        RUVIA_CHECK(received.status == ruvia::quic_datagram_status::received);
        RUVIA_CHECK_EQ(received.size, std::size_t{3});
        RUVIA_CHECK_EQ(output[0], std::byte{'m'});
        RUVIA_CHECK_EQ(output[2], std::byte{'g'});
        RUVIA_CHECK_EQ(state.received_datagram_bytes_, std::size_t{0});
        RUVIA_CHECK_EQ(resource.live_bytes, before_receive);
    }
    RUVIA_CHECK_EQ(resource.live_blocks, std::size_t{0});
    RUVIA_CHECK_EQ(resource.live_bytes, std::size_t{0});
}

void test_callback_allocation_failure_latches_without_partial_retention(
    ruvia::testing::TestContext& ruvia_ctx) {
    counting_resource resource;
    {
        auto test_config = config();
        test_config.limits.max_stream_buffer_size = 64;
        test_config.limits.max_connection_buffer_size = 64;
        test_config.local_transport_parameters.initial_max_data = 64;
        test_config.local_transport_parameters.initial_max_stream_data_bidi_local = 64;
        test_config.local_transport_parameters.initial_max_stream_data_bidi_remote = 64;
        test_config.local_transport_parameters.initial_max_stream_data_uni = 64;
        ruvia::detail::quic_connection_state state(std::move(test_config), provider(),
            {.drive = drive_tls, .retire = retire_tls}, &resource, {});
        const auto stream_id = open_peer_bidi(state, ruvia_ctx);
        resource.allocations_before_failure = 0;
        const int result = ruvia::detail::quic_stream_data_callback(nullptr, 0,
            static_cast<std::int64_t>(stream_id), 0,
            reinterpret_cast<const std::uint8_t*>("12345678901234567890123456789012"), 32,
            &state, nullptr);
        RUVIA_CHECK_EQ(result, NGTCP2_ERR_CALLBACK_FAILURE);
        RUVIA_CHECK(state.failed());
        RUVIA_CHECK_EQ(state.retained_stream_input_bytes_, std::size_t{0});
    }
    RUVIA_CHECK_EQ(resource.live_blocks, std::size_t{0});
    RUVIA_CHECK_EQ(resource.live_bytes, std::size_t{0});
}

}  // namespace

RUVIA_TEST(quic_stream_early_data_provenance_is_sticky_transport_state) {
    counting_resource resource;
    ruvia::detail::quic_connection_state state(config(), provider(),
        {.drive = drive_tls, .retire = retire_tls}, &resource, {});
    const auto stream_id = open_peer_bidi(state, ruvia_ctx);
    RUVIA_CHECK_EQ(ruvia::detail::quic_stream_data_callback(nullptr,
                       NGTCP2_STREAM_DATA_FLAG_0RTT,
                       static_cast<std::int64_t>(stream_id), 0,
                       reinterpret_cast<const std::uint8_t*>("four"), 4, &state, nullptr),
        0);
    RUVIA_CHECK(ruvia::detail::stream_received_early_data(state, stream_id));
    RUVIA_CHECK_EQ(ruvia::detail::quic_stream_data_callback(nullptr, 0,
                       static_cast<std::int64_t>(stream_id), 4,
                       reinterpret_cast<const std::uint8_t*>("late"), 4, &state, nullptr),
        0);
    RUVIA_CHECK(ruvia::detail::stream_received_early_data(state, stream_id));
}

RUVIA_TEST(quic_stream_early_data_rejection_retires_buffers_and_reports_invalidated_streams) {
    counting_resource resource;
    auto test_config = config();
    test_config.role = ruvia::quic_role::server;
    test_config.original_destination_connection_id = test_config.destination_connection_id;
    ruvia::detail::quic_connection_state state(std::move(test_config), provider(),
        {.drive = drive_tls, .retire = retire_tls}, &resource, {});
    constexpr std::uint64_t stream_id = 0;
    RUVIA_CHECK_EQ(ruvia::detail::quic_stream_open_callback(nullptr,
                       static_cast<std::int64_t>(stream_id), &state),
        0);
    RUVIA_CHECK_EQ(ruvia::detail::quic_stream_data_callback(nullptr,
                       NGTCP2_STREAM_DATA_FLAG_0RTT,
                       static_cast<std::int64_t>(stream_id), 0,
                       reinterpret_cast<const std::uint8_t*>("body"), 4, &state, nullptr),
        0);
    const auto queued = ruvia::detail::queue_stream_write(state, stream_id,
        bytes("out", 3), true);
    RUVIA_CHECK(queued.status == ruvia::quic_operation_status::accepted);
    RUVIA_CHECK_EQ(state.retained_stream_input_bytes_, std::size_t{4});
    RUVIA_CHECK_EQ(state.retained_stream_output_bytes_, std::size_t{3});

    state.tls_driver_active_ = true;
    state.tls_handshake().complete_early_data(false);
    state.tls_driver_active_ = false;
    RUVIA_CHECK(!state.streams_.contains(stream_id));
    RUVIA_CHECK_EQ(state.retained_stream_input_bytes_, std::size_t{0});
    RUVIA_CHECK_EQ(state.retained_stream_output_bytes_, std::size_t{0});
    RUVIA_CHECK_EQ(state.rejected_early_streams_.size(), std::size_t{1});
    RUVIA_CHECK_EQ(state.rejected_early_streams_.front(), stream_id);
    RUVIA_CHECK(state.info().early_data == ruvia::quic_early_data_state::rejected);
}

RUVIA_TEST(quic_stream_callbacks_preserve_input_and_replenish_consumed_buffers) {
    test_receive_copy_consume_and_append(ruvia_ctx);
}

RUVIA_TEST(quic_stream_send_helpers_schedule_fairly_and_release_acknowledged_ranges) {
    test_send_ranges_ack_release_and_fair_selection(ruvia_ctx);
}

RUVIA_TEST(quic_stream_and_datagram_queues_enforce_budgets_and_empty_values) {
    test_stream_and_datagram_budgets_empty_values_and_cold_release(ruvia_ctx);
}

RUVIA_TEST(quic_stream_callback_latches_allocation_failure_transactionally) {
    test_callback_allocation_failure_latches_without_partial_retention(ruvia_ctx);
}

RUVIA_TEST(quic_stream_retired_owner_accepts_peer_reset_and_stop_until_native_close) {
    counting_resource resource;
    {
        ruvia::detail::quic_connection_state state(config(), provider(),
            {.drive = drive_tls, .retire = retire_tls}, &resource, {});
        const auto id = open_peer_bidi(state, ruvia_ctx);
        auto& stream = state.streams_.at(id);
        stream.retired = true;
        stream.receive_end_observed = true;
        RUVIA_CHECK_EQ(ruvia::detail::quic_stream_reset_callback(nullptr,
                           static_cast<std::int64_t>(id), 8, 268, &state, nullptr),
            0);
        RUVIA_CHECK_EQ(stream.received_offset, std::uint64_t{8});
        RUVIA_CHECK_EQ(stream.peer_reset_error, std::optional<std::uint64_t>{268});
        RUVIA_CHECK_EQ(ruvia::detail::quic_stream_stop_sending_callback(nullptr,
                           static_cast<std::int64_t>(id), 268, &state, nullptr),
            0);
        RUVIA_CHECK(stream.send_stopped);
        RUVIA_CHECK(!state.latched_failure_);
        RUVIA_CHECK_EQ(ruvia::detail::quic_stream_close_callback(nullptr, 0,
                           static_cast<std::int64_t>(id), 0, 0, &state, nullptr),
            0);
        RUVIA_CHECK(state.streams_.empty());
    }
    RUVIA_CHECK_EQ(resource.live_bytes, std::size_t{0});
}
