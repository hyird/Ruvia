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

// Throws invalid_stream for an unknown or retired stream.
quic_connection_state::stream_state& require_quic_stream(quic_connection_state& state_value,
    std::uint64_t stream_id);
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

void configure_quic_stream_callbacks(ngtcp2_callbacks& callbacks) noexcept;

}  // namespace ruvia::detail
