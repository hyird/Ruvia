#pragma once

#include <ngtcp2/ngtcp2.h>

#include <cstddef>
#include <cstdint>
#include <span>

#include "http3/quic_connection_state.h"

namespace ruvia::detail {

struct quic_datagram_write_view final {
    std::uint64_t id_{};
    std::span<const std::byte> bytes_{};
    bool available_{};

    explicit operator bool() const noexcept {
        return available_;
    }
};

std::size_t maximum_datagram_payload_size(const quic_connection_state& state_value) noexcept;
quic_datagram_write_status queue_datagram(quic_connection_state& state_value,
    std::span<const std::byte> payload_value);
// The returned payload borrows the queue front until commit or another queue mutation.
quic_datagram_write_view next_datagram_write(quic_connection_state& state_value) noexcept;
void commit_datagram_write(quic_connection_state& state_value, std::uint64_t id, bool accepted) noexcept;
quic_datagram_result read_datagram_buffer(quic_connection_state& state_value,
    std::span<std::byte> output) noexcept;

void configure_quic_datagram_callbacks(ngtcp2_callbacks& callbacks) noexcept;

}  // namespace ruvia::detail
