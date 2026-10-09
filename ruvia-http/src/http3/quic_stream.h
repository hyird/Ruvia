#pragma once

#include <ngtcp2/ngtcp2.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "http3/quic_connection_state.h"

namespace ruvia::detail {

inline constexpr std::size_t quic_stream_write_range_count = 8;

struct quic_stream_write_view final {
    std::uint64_t stream_id_{};
    std::array<std::span<const std::byte>, quic_stream_write_range_count> ranges_{};
    std::size_t range_count_{};
    bool fin_{};

    explicit operator bool() const noexcept {
        return range_count_ != 0 || fin_;
    }
};

struct quic_datagram_write_view final {
    std::uint64_t id_{};
    std::span<const std::byte> bytes_{};
    bool available_{};

    explicit operator bool() const noexcept {
        return available_;
    }
};

quic_stream_write_result queue_stream_write(quic_connection_state& state_value,
    std::uint64_t stream_id, std::span<const std::byte> input, bool fin);
quic_stream_read_result read_stream_buffer(quic_connection_state& state_value,
    std::uint64_t stream_id, std::span<std::byte> output);
quic_stream_read_result inspect_stream_read(const quic_connection_state& state_value,
    std::uint64_t stream_id) noexcept;
[[nodiscard]] bool stream_received_early_data(
    const quic_connection_state& state_value, std::uint64_t stream_id) noexcept;
quic_operation_status inspect_stream_write(const quic_connection_state& state_value,
    std::uint64_t stream_id) noexcept;
// The returned spans borrow queued blocks until any stream-buffer mutation.
quic_stream_write_view next_stream_write(quic_connection_state& state_value) noexcept;
void commit_stream_write(quic_connection_state& state_value, const quic_stream_write_view& offered,
    std::size_t accepted, bool fin_submitted);
// offset + size is ngtcp2's cumulative, gap-free ACK watermark, not a wire ACK range.
std::size_t acknowledge_stream_data_through(quic_connection_state& state_value,
    std::uint64_t stream_id, std::uint64_t offset, std::uint64_t size);
void retire_closed_streams(quic_connection_state& state_value, ngtcp2_conn* connection) noexcept;

std::size_t maximum_datagram_payload_size(const quic_connection_state& state_value) noexcept;
quic_datagram_write_status queue_datagram(quic_connection_state& state_value,
    std::span<const std::byte> payload_value);
quic_datagram_write_status queue_datagram_with_limit(quic_connection_state& state_value,
    std::span<const std::byte> payload_value, std::size_t payload_limit);
// The returned payload borrows the queue front until commit or another queue mutation.
quic_datagram_write_view next_datagram_write(quic_connection_state& state_value) noexcept;
quic_datagram_write_view next_datagram_write_with_limit(quic_connection_state& state_value,
    std::size_t payload_limit) noexcept;
void commit_datagram_write(quic_connection_state& state_value, std::uint64_t id, bool accepted) noexcept;
quic_datagram_result read_datagram_buffer(quic_connection_state& state_value,
    std::span<std::byte> output) noexcept;

int quic_stream_open_callback(ngtcp2_conn* connection, std::int64_t stream_id,
    void* user_data) noexcept;
int quic_stream_data_callback(ngtcp2_conn* connection, std::uint32_t flags,
    std::int64_t stream_id, std::uint64_t offset, const std::uint8_t* data,
    std::size_t size, void* user_data, void* stream_user_data) noexcept;
int quic_stream_reset_callback(ngtcp2_conn* connection, std::int64_t stream_id,
    std::uint64_t final_size, std::uint64_t error_code, void* user_data,
    void* stream_user_data) noexcept;
int quic_stream_stop_sending_callback(ngtcp2_conn* connection, std::int64_t stream_id,
    std::uint64_t error_code, void* user_data, void* stream_user_data) noexcept;
int quic_stream_close_callback(ngtcp2_conn* connection, std::uint32_t flags,
    std::int64_t stream_id, std::uint64_t receive_error_code,
    std::uint64_t send_error_code, void* user_data, void* stream_user_data) noexcept;
int quic_stream_acknowledged_callback(ngtcp2_conn* connection,
    std::int64_t stream_id, std::uint64_t offset, std::uint64_t size,
    void* user_data, void* stream_user_data) noexcept;
int quic_datagram_received_callback(ngtcp2_conn* connection, std::uint32_t flags,
    const std::uint8_t* data, std::size_t size, void* user_data) noexcept;
int quic_datagram_acknowledged_callback(ngtcp2_conn* connection,
    std::uint64_t datagram_id, void* user_data) noexcept;
int quic_datagram_lost_callback(ngtcp2_conn* connection,
    std::uint64_t datagram_id, void* user_data) noexcept;
void configure_quic_stream_callbacks(ngtcp2_callbacks& callbacks) noexcept;

}  // namespace ruvia::detail
