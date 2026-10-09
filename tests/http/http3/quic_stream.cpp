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
    std::size_t live_bytes_{};
    std::size_t live_blocks_{};
    std::size_t allocations_before_failure_{std::numeric_limits<std::size_t>::max()};

private:
    void* do_allocate(std::size_t size, std::size_t alignment) override {
        if (allocations_before_failure_ == 0) {
            allocations_before_failure_ = std::numeric_limits<std::size_t>::max();
            throw std::bad_alloc();
        }
        if (allocations_before_failure_ != std::numeric_limits<std::size_t>::max()) {
            --allocations_before_failure_;
        }
        void* const result_value = std::pmr::new_delete_resource()->allocate(size, alignment);
        live_bytes_ += size;
        ++live_blocks_;
        return result_value;
    }

    void do_deallocate(void* pointer, std::size_t size, std::size_t alignment) override {
        live_bytes_ -= size;
        --live_blocks_;
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
    return {.random_bytes_ = random_bytes,
        .hkdf_extract_ = hkdf_extract,
        .hkdf_expand_ = hkdf_expand,
        .create_aead_key_ = create_aead,
        .create_header_protection_key_ = create_header,
        .secure_erase_ = secure_erase};
}

ruvia::quic_connection_config config() {
    ruvia::quic_connection_config result;
    result.local_address_.port_ = 443;
    result.peer_address_.port_ = 50000;
    const std::array dcid{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4},
        std::byte{5}, std::byte{6}, std::byte{7}, std::byte{8}};
    const std::array scid{std::byte{8}, std::byte{7}, std::byte{6}, std::byte{5},
        std::byte{4}, std::byte{3}, std::byte{2}, std::byte{1}};
    result.destination_connection_id_ = ruvia::quic_connection_id(dcid);
    result.source_connection_id_ = ruvia::quic_connection_id(scid);
    result.limits_.max_stream_buffer_size_ = 8;
    result.limits_.max_connection_buffer_size_ = 32;
    result.local_transport_parameters_.initial_max_data_ = 32;
    result.local_transport_parameters_.initial_max_stream_data_bidi_local_ = 8;
    result.local_transport_parameters_.initial_max_stream_data_bidi_remote_ = 8;
    result.local_transport_parameters_.initial_max_stream_data_uni_ = 8;
    result.local_transport_parameters_.initial_max_streams_bidi_ = 2;
    result.local_transport_parameters_.initial_max_streams_uni_ = 2;
    result.limits_.max_datagram_size_ = 1200;
    result.limits_.max_datagrams_ = 2;
    return result;
}

std::span<const std::byte> bytes(const char* value, std::size_t size) {
    return {reinterpret_cast<const std::byte*>(value), size};
}

std::uint64_t open_peer_bidi(ruvia::detail::quic_connection_state& state_value,
    ruvia::testing::test_context& ruvia_ctx) {
    constexpr std::uint64_t stream_id = 1;
    RUVIA_CHECK_EQ(ruvia::detail::quic_stream_open_callback(nullptr,
                       static_cast<std::int64_t>(stream_id), &state_value),
        0);
    return stream_id;
}

void test_receive_copy_consume_and_append(ruvia::testing::test_context& ruvia_ctx) {
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
        ruvia::detail::quic_connection_state state_value(config(), provider(),
            {.drive_ = drive_tls, .retire_ = retire_tls}, &resource, {});
        const auto stream_id = open_peer_bidi(state_value, ruvia_ctx);
        RUVIA_CHECK_EQ(ruvia::detail::quic_stream_data_callback(nullptr, 0,
                           static_cast<std::int64_t>(stream_id), 0,
                           reinterpret_cast<const std::uint8_t*>("abcd"), 4, &state_value, nullptr),
            0);

        std::array<std::byte, 2> first{};
        const auto first_read = ruvia::detail::read_stream_buffer(state_value, stream_id, first);
        RUVIA_CHECK(first_read.status_ == ruvia::quic_stream_read_status::data);
        RUVIA_CHECK_EQ(first_read.size_, std::size_t{2});
        RUVIA_CHECK_EQ(first[0], std::byte{'a'});
        RUVIA_CHECK_EQ(first[1], std::byte{'b'});
        RUVIA_CHECK_EQ(state_value.retained_stream_input_bytes_, std::size_t{2});

        RUVIA_CHECK_EQ(ruvia::detail::quic_stream_data_callback(nullptr, 0,
                           static_cast<std::int64_t>(stream_id), 4,
                           reinterpret_cast<const std::uint8_t*>("efgh"), 4, &state_value, nullptr),
            0);
        std::array<std::byte, 8> rest{};
        const auto rest_read = ruvia::detail::read_stream_buffer(state_value, stream_id, rest);
        RUVIA_CHECK_EQ(rest_read.size_, std::size_t{6});
        RUVIA_CHECK_EQ(rest[0], std::byte{'c'});
        RUVIA_CHECK_EQ(rest[1], std::byte{'d'});
        RUVIA_CHECK_EQ(rest[2], std::byte{'e'});
        RUVIA_CHECK_EQ(rest[5], std::byte{'h'});
        RUVIA_CHECK_EQ(state_value.retained_stream_input_bytes_, std::size_t{0});

        RUVIA_CHECK_EQ(ruvia::detail::quic_stream_data_callback(nullptr,
                           NGTCP2_STREAM_DATA_FLAG_FIN, static_cast<std::int64_t>(stream_id),
                           8, nullptr, 0, &state_value, nullptr),
            0);
        const auto eof = ruvia::detail::read_stream_buffer(state_value, stream_id, rest);
        RUVIA_CHECK(eof.status_ == ruvia::quic_stream_read_status::fin);
    }
    RUVIA_CHECK_EQ(resource.live_blocks_, std::size_t{0});
    RUVIA_CHECK_EQ(resource.live_bytes_, std::size_t{0});
}

void test_send_ranges_ack_release_and_fair_selection(ruvia::testing::test_context& ruvia_ctx) {
    counting_resource resource;
    {
        auto test_config = config();
        test_config.limits_.max_stream_buffer_size_ = 64;
        test_config.limits_.max_connection_buffer_size_ = 128;
        test_config.local_transport_parameters_.initial_max_data_ = 128;
        test_config.local_transport_parameters_.initial_max_stream_data_bidi_local_ = 64;
        test_config.local_transport_parameters_.initial_max_stream_data_bidi_remote_ = 64;
        test_config.local_transport_parameters_.initial_max_stream_data_uni_ = 64;
        ruvia::detail::quic_connection_state state_value(std::move(test_config), provider(),
            {.drive_ = drive_tls, .retire_ = retire_tls}, &resource, {});
        state_value.streams_.try_emplace(0, &resource);
        auto& first = state_value.streams_.at(0);
        first.readable_ = true;
        first.writable_ = true;
        state_value.streams_.try_emplace(4, &resource);
        auto& second = state_value.streams_.at(4);
        second.readable_ = true;
        second.writable_ = true;

        constexpr char first_bytes[] = "abcdefghijklmnopqrstuvwxyzABCDEF";
        constexpr char second_bytes[] = "01234567890123456789012345678901";
        constexpr char repeated_bytes[] = "ZYXWVUTSRQPONMLKJIHGFEDCBA987654";
        const auto baseline_bytes = resource.live_bytes_;
        auto first_write = ruvia::detail::queue_stream_write(state_value, 0, bytes(first_bytes, 32), false);
        RUVIA_CHECK_EQ(first_write.accepted_, std::size_t{32});
        RUVIA_CHECK_EQ(ruvia::detail::queue_stream_write(state_value, 0, bytes(second_bytes, 32), false).accepted_,
            std::size_t{32});
        RUVIA_CHECK_EQ(ruvia::detail::queue_stream_write(state_value, 4, bytes(second_bytes, 32), false).accepted_,
            std::size_t{32});
        const auto retained_bytes = resource.live_bytes_;
        RUVIA_CHECK(retained_bytes > baseline_bytes);

        auto selected = ruvia::detail::next_stream_write(state_value);
        RUVIA_CHECK_EQ(selected.stream_id_, std::uint64_t{0});
        RUVIA_CHECK_EQ(selected.range_count_, std::size_t{2});
        const auto second_block_pointer = selected.ranges_[1].data();
        ruvia::detail::commit_stream_write(state_value, selected, 64, false);
        auto fair = ruvia::detail::next_stream_write(state_value);
        RUVIA_CHECK_EQ(fair.stream_id_, std::uint64_t{4});
        ruvia::detail::commit_stream_write(state_value, fair, 32, false);

        RUVIA_CHECK_EQ(ruvia::detail::quic_stream_acknowledged_callback(nullptr, 0, 0, 16,
                           &state_value, nullptr),
            0);
        RUVIA_CHECK_EQ(state_value.retained_stream_output_bytes_, std::size_t{96});
        RUVIA_CHECK_EQ(ruvia::detail::quic_stream_acknowledged_callback(nullptr, 0, 16, 16,
                           &state_value, nullptr),
            0);
        RUVIA_CHECK_EQ(state_value.retained_stream_output_bytes_, std::size_t{64});
        const auto& retained_block = state_value.streams_.at(0).output_.front();
        RUVIA_CHECK_EQ(reinterpret_cast<const std::byte*>(retained_block.bytes_.data()),
            second_block_pointer);
        RUVIA_CHECK_EQ(retained_block.bytes_.size(), std::size_t{32});
        RUVIA_CHECK_EQ(ruvia::detail::quic_stream_acknowledged_callback(nullptr, 0, 32, 32,
                           &state_value, nullptr),
            0);
        RUVIA_CHECK_EQ(state_value.retained_stream_output_bytes_, std::size_t{32});
        RUVIA_CHECK_EQ(ruvia::detail::quic_stream_acknowledged_callback(nullptr, 4, 0, 32,
                           &state_value, nullptr),
            0);
        RUVIA_CHECK_EQ(state_value.retained_stream_output_bytes_, std::size_t{0});
        RUVIA_CHECK(resource.live_bytes_ < retained_bytes);
        RUVIA_CHECK_EQ(ruvia::detail::quic_stream_close_callback(nullptr, 0, 0, 0, 0,
                           &state_value, nullptr),
            0);
        RUVIA_CHECK_EQ(state_value.retained_stream_output_bytes_, std::size_t{0});

        RUVIA_CHECK(resource.live_bytes_ < retained_bytes);
        std::size_t plateau_bytes{};
        for (std::size_t iteration = 0; iteration < 128; ++iteration) {
            const auto offset = state_value.streams_.at(4).send_offset_;
            const auto repeated = ruvia::detail::queue_stream_write(
                state_value, 4, bytes(repeated_bytes, 32), false);
            RUVIA_CHECK_EQ(repeated.accepted_, std::size_t{32});
            const auto repeated_write = ruvia::detail::next_stream_write(state_value);
            RUVIA_CHECK_EQ(repeated_write.stream_id_, std::uint64_t{4});
            ruvia::detail::commit_stream_write(state_value, repeated_write, 32, false);
            RUVIA_CHECK_EQ(ruvia::detail::quic_stream_acknowledged_callback(
                               nullptr, 4, offset, 12, &state_value, nullptr),
                0);
            RUVIA_CHECK_EQ(state_value.retained_stream_output_bytes_, std::size_t{32});
            RUVIA_CHECK_EQ(ruvia::detail::quic_stream_acknowledged_callback(
                               nullptr, 4, offset + 12, 20, &state_value, nullptr),
                0);
            RUVIA_CHECK_EQ(state_value.retained_stream_output_bytes_, std::size_t{0});
            if (iteration == 0) {
                plateau_bytes = resource.live_bytes_;
            } else {
                RUVIA_CHECK_EQ(resource.live_bytes_, plateau_bytes);
            }
        }

        state_value.streams_.try_emplace(8, &resource);
        auto& empty_fin_stream = state_value.streams_.at(8);
        empty_fin_stream.writable_ = true;
        RUVIA_CHECK(ruvia::detail::queue_stream_write(state_value, 8, {}, true).status_ ==
                    ruvia::quic_operation_status::accepted);
        const auto empty_fin = ruvia::detail::next_stream_write(state_value);
        RUVIA_CHECK_EQ(empty_fin.stream_id_, std::uint64_t{8});
        RUVIA_CHECK(empty_fin.fin_);
        ruvia::detail::commit_stream_write(state_value, empty_fin, 0, true);
        const auto other_offset = state_value.streams_.at(4).send_offset_;
        RUVIA_CHECK_EQ(ruvia::detail::queue_stream_write(
                           state_value, 4, bytes(second_bytes, 32), false)
                           .accepted_,
            std::size_t{32});
        const auto other_write = ruvia::detail::next_stream_write(state_value);
        RUVIA_CHECK_EQ(other_write.stream_id_, std::uint64_t{4});
        ruvia::detail::commit_stream_write(state_value, other_write, 32, false);
        RUVIA_CHECK_EQ(ruvia::detail::quic_stream_acknowledged_callback(
                           nullptr, 8, 0, 0, &state_value, nullptr),
            0);
        RUVIA_CHECK_EQ(state_value.retained_stream_output_bytes_, std::size_t{32});
        RUVIA_CHECK_EQ(ruvia::detail::quic_stream_acknowledged_callback(
                           nullptr, 4, other_offset, 32, &state_value, nullptr),
            0);
        RUVIA_CHECK_EQ(state_value.retained_stream_output_bytes_, std::size_t{0});
    }
    RUVIA_CHECK_EQ(resource.live_blocks_, std::size_t{0});
    RUVIA_CHECK_EQ(resource.live_bytes_, std::size_t{0});
}

void test_stream_and_datagram_budgets_empty_values_and_cold_release(
    ruvia::testing::test_context& ruvia_ctx) {
    counting_resource resource;
    {
        ruvia::detail::quic_connection_state state_value(config(), provider(),
            {.drive_ = drive_tls, .retire_ = retire_tls}, &resource, {});
        const auto stream_id = open_peer_bidi(state_value, ruvia_ctx);
        RUVIA_CHECK_EQ(ruvia::detail::queue_stream_write(state_value, stream_id,
                           bytes("12345678", 8), false)
                           .accepted_,
            std::size_t{8});
        const auto blocked = ruvia::detail::queue_stream_write(state_value, stream_id,
            bytes("x", 1), false);
        RUVIA_CHECK(blocked.status_ == ruvia::quic_operation_status::would_block);
        RUVIA_CHECK_EQ(state_value.retained_stream_output_bytes_, std::size_t{8});

        RUVIA_CHECK(ruvia::detail::queue_datagram_with_limit(state_value, {}, 12) ==
                    ruvia::quic_datagram_write_status::queued);
        RUVIA_CHECK(ruvia::detail::queue_datagram_with_limit(state_value, bytes("abcd", 4), 12) ==
                    ruvia::quic_datagram_write_status::queued);
        RUVIA_CHECK(ruvia::detail::queue_datagram_with_limit(state_value, bytes("toolarge", 8), 4) ==
                    ruvia::quic_datagram_write_status::too_large);
        RUVIA_CHECK(ruvia::detail::queue_datagram_with_limit(state_value, bytes("w", 1), 12) ==
                    ruvia::quic_datagram_write_status::dropped);
        auto empty = ruvia::detail::next_datagram_write_with_limit(state_value, 12);
        RUVIA_CHECK(empty.available_);
        RUVIA_CHECK_EQ(empty.id_, std::uint64_t{0});
        RUVIA_CHECK(empty.bytes_.empty());
        ruvia::detail::commit_datagram_write(state_value, empty.id_, true);
        RUVIA_CHECK(ruvia::detail::queue_datagram_with_limit(state_value, bytes("z", 1), 12) ==
                    ruvia::quic_datagram_write_status::queued);
        auto next_value = ruvia::detail::next_datagram_write_with_limit(state_value, 2);
        RUVIA_CHECK(next_value.available_);
        RUVIA_CHECK_EQ(next_value.bytes_.size(), std::size_t{1});
        RUVIA_CHECK_EQ(next_value.bytes_[0], std::byte{'z'});
        ruvia::detail::commit_datagram_write(state_value, next_value.id_, false);
        RUVIA_CHECK_EQ(state_value.send_datagram_bytes_, std::size_t{1});
        next_value = ruvia::detail::next_datagram_write_with_limit(state_value, 12);
        RUVIA_CHECK(next_value.available_);
        RUVIA_CHECK_EQ(next_value.bytes_.size(), std::size_t{1});
        ruvia::detail::commit_datagram_write(state_value, next_value.id_, true);
        RUVIA_CHECK_EQ(state_value.send_datagram_bytes_, std::size_t{0});

        RUVIA_CHECK_EQ(ruvia::detail::quic_datagram_received_callback(nullptr, 0,
                           nullptr, 0, &state_value),
            0);
        std::array<std::byte, 4> output{};
        const auto empty_received = ruvia::detail::read_datagram_buffer(state_value, output);
        RUVIA_CHECK(empty_received.status_ == ruvia::quic_datagram_status::received);
        RUVIA_CHECK_EQ(empty_received.size_, std::size_t{0});
        RUVIA_CHECK(ruvia::detail::read_datagram_buffer(state_value, output).status_ ==
                    ruvia::quic_datagram_status::would_block);
        const auto before_receive = resource.live_bytes_;
        RUVIA_CHECK_EQ(ruvia::detail::quic_datagram_received_callback(nullptr, 0,
                           reinterpret_cast<const std::uint8_t*>("msg"), 3, &state_value),
            0);
        std::array<std::byte, 2> too_small{};
        RUVIA_CHECK(ruvia::detail::read_datagram_buffer(state_value, too_small).status_ ==
                    ruvia::quic_datagram_status::too_large);
        RUVIA_CHECK_EQ(state_value.received_datagram_bytes_, std::size_t{3});
        const auto received_value = ruvia::detail::read_datagram_buffer(state_value, output);
        RUVIA_CHECK(received_value.status_ == ruvia::quic_datagram_status::received);
        RUVIA_CHECK_EQ(received_value.size_, std::size_t{3});
        RUVIA_CHECK_EQ(output[0], std::byte{'m'});
        RUVIA_CHECK_EQ(output[2], std::byte{'g'});
        RUVIA_CHECK_EQ(state_value.received_datagram_bytes_, std::size_t{0});
        RUVIA_CHECK_EQ(resource.live_bytes_, before_receive);
    }
    RUVIA_CHECK_EQ(resource.live_blocks_, std::size_t{0});
    RUVIA_CHECK_EQ(resource.live_bytes_, std::size_t{0});
}

void test_callback_allocation_failure_latches_without_partial_retention(
    ruvia::testing::test_context& ruvia_ctx) {
    counting_resource resource;
    {
        auto test_config = config();
        test_config.limits_.max_stream_buffer_size_ = 64;
        test_config.limits_.max_connection_buffer_size_ = 64;
        test_config.local_transport_parameters_.initial_max_data_ = 64;
        test_config.local_transport_parameters_.initial_max_stream_data_bidi_local_ = 64;
        test_config.local_transport_parameters_.initial_max_stream_data_bidi_remote_ = 64;
        test_config.local_transport_parameters_.initial_max_stream_data_uni_ = 64;
        ruvia::detail::quic_connection_state state_value(std::move(test_config), provider(),
            {.drive_ = drive_tls, .retire_ = retire_tls}, &resource, {});
        const auto stream_id = open_peer_bidi(state_value, ruvia_ctx);
        resource.allocations_before_failure_ = 0;
        const int result_value = ruvia::detail::quic_stream_data_callback(nullptr, 0,
            static_cast<std::int64_t>(stream_id), 0,
            reinterpret_cast<const std::uint8_t*>("12345678901234567890123456789012"), 32,
            &state_value, nullptr);
        RUVIA_CHECK_EQ(result_value, NGTCP2_ERR_CALLBACK_FAILURE);
        RUVIA_CHECK(state_value.failed());
        RUVIA_CHECK_EQ(state_value.retained_stream_input_bytes_, std::size_t{0});
    }
    RUVIA_CHECK_EQ(resource.live_blocks_, std::size_t{0});
    RUVIA_CHECK_EQ(resource.live_bytes_, std::size_t{0});
}

}  // namespace

RUVIA_TEST(quic_stream_early_data_provenance_is_sticky_transport_state) {
    counting_resource resource;
    ruvia::detail::quic_connection_state state_value(config(), provider(),
        {.drive_ = drive_tls, .retire_ = retire_tls}, &resource, {});
    const auto stream_id = open_peer_bidi(state_value, ruvia_ctx);
    RUVIA_CHECK_EQ(ruvia::detail::quic_stream_data_callback(nullptr,
                       NGTCP2_STREAM_DATA_FLAG_0RTT,
                       static_cast<std::int64_t>(stream_id), 0,
                       reinterpret_cast<const std::uint8_t*>("four"), 4, &state_value, nullptr),
        0);
    RUVIA_CHECK(ruvia::detail::stream_received_early_data(state_value, stream_id));
    RUVIA_CHECK_EQ(ruvia::detail::quic_stream_data_callback(nullptr, 0,
                       static_cast<std::int64_t>(stream_id), 4,
                       reinterpret_cast<const std::uint8_t*>("late"), 4, &state_value, nullptr),
        0);
    RUVIA_CHECK(ruvia::detail::stream_received_early_data(state_value, stream_id));
}

RUVIA_TEST(quic_stream_early_data_rejection_retires_buffers_and_reports_invalidated_streams) {
    counting_resource resource;
    auto test_config = config();
    test_config.role_ = ruvia::quic_role::server;
    test_config.original_destination_connection_id_ = test_config.destination_connection_id_;
    ruvia::detail::quic_connection_state state_value(std::move(test_config), provider(),
        {.drive_ = drive_tls, .retire_ = retire_tls}, &resource, {});
    constexpr std::uint64_t stream_id = 0;
    RUVIA_CHECK_EQ(ruvia::detail::quic_stream_open_callback(nullptr,
                       static_cast<std::int64_t>(stream_id), &state_value),
        0);
    RUVIA_CHECK_EQ(ruvia::detail::quic_stream_data_callback(nullptr,
                       NGTCP2_STREAM_DATA_FLAG_0RTT,
                       static_cast<std::int64_t>(stream_id), 0,
                       reinterpret_cast<const std::uint8_t*>("body"), 4, &state_value, nullptr),
        0);
    const auto queued = ruvia::detail::queue_stream_write(state_value, stream_id,
        bytes("out", 3), true);
    RUVIA_CHECK(queued.status_ == ruvia::quic_operation_status::accepted);
    RUVIA_CHECK_EQ(state_value.retained_stream_input_bytes_, std::size_t{4});
    RUVIA_CHECK_EQ(state_value.retained_stream_output_bytes_, std::size_t{3});

    state_value.tls_driver_active_ = true;
    state_value.tls_handshake().complete_early_data(false);
    state_value.tls_driver_active_ = false;
    RUVIA_CHECK(!state_value.streams_.contains(stream_id));
    RUVIA_CHECK_EQ(state_value.retained_stream_input_bytes_, std::size_t{0});
    RUVIA_CHECK_EQ(state_value.retained_stream_output_bytes_, std::size_t{0});
    RUVIA_CHECK_EQ(state_value.rejected_early_streams_.size(), std::size_t{1});
    RUVIA_CHECK_EQ(state_value.rejected_early_streams_.front(), stream_id);
    RUVIA_CHECK(state_value.info().early_data_ == ruvia::quic_early_data_state::rejected);
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
        ruvia::detail::quic_connection_state state_value(config(), provider(),
            {.drive_ = drive_tls, .retire_ = retire_tls}, &resource, {});
        const auto id = open_peer_bidi(state_value, ruvia_ctx);
        auto& stream = state_value.streams_.at(id);
        stream.retired_ = true;
        stream.receive_end_observed_ = true;
        RUVIA_CHECK_EQ(ruvia::detail::quic_stream_reset_callback(nullptr,
                           static_cast<std::int64_t>(id), 8, 268, &state_value, nullptr),
            0);
        RUVIA_CHECK_EQ(stream.received_offset_, std::uint64_t{8});
        RUVIA_CHECK_EQ(stream.peer_reset_error_, std::optional<std::uint64_t>{268});
        RUVIA_CHECK_EQ(ruvia::detail::quic_stream_stop_sending_callback(nullptr,
                           static_cast<std::int64_t>(id), 268, &state_value, nullptr),
            0);
        RUVIA_CHECK(stream.send_stopped_);
        RUVIA_CHECK(!state_value.latched_failure_);
        RUVIA_CHECK_EQ(ruvia::detail::quic_stream_close_callback(nullptr, 0,
                           static_cast<std::int64_t>(id), 0, 0, &state_value, nullptr),
            0);
        RUVIA_CHECK(state_value.streams_.empty());
    }
    RUVIA_CHECK_EQ(resource.live_bytes_, std::size_t{0});
}
