#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>

#include "ruvia/http/quic_types.h"
#include "ruvia/web/server_config.h"

namespace ruvia::detail {

// Checked startup dimensions shared by application, standalone workers and Acceptor.
struct http3_capacity final {
    // One pool slot holds one UDP payload. 1472 bytes is the largest payload of
    // a 1500-byte Ethernet MTU: it covers peers' padded Initials above the
    // 1200-byte minimum and every packet the server transmits, which never
    // exceeds its advertised max_udp_payload_size. A larger datagram arrives
    // truncated and is dropped like any other malformed packet.
    static constexpr std::size_t packet_bytes = 1472;
    // Receive leases the Acceptor keeps armed or holding routed-pending packets.
    static constexpr std::size_t receive_slots = 32;
    // Native sends the Acceptor keeps in flight. Their leases are worker output
    // credits, so this bounds socket metadata only, never pool storage.
    static constexpr std::size_t send_slots = 64;
    std::size_t input_slots_;
    std::size_t output_credits_;
    std::size_t packet_slots_;
};

static_assert(http3_capacity::packet_bytes >= ruvia::quic_transport_parameters{}.max_udp_payload_size_,
    "an Acceptor packet slot must hold the largest UDP payload the server transmits");

[[nodiscard]] inline http3_capacity normalize_http3_capacity(
    const http3_listen_config& config, std::size_t worker_count) {
    if (worker_count == 0 || worker_count > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument("HTTP/3 worker count is not representable");
    }
    const auto input = config.datagram_input_capacity_;
    const auto output = config.datagram_output_capacity_;
    constexpr auto max_slots = static_cast<std::size_t>(
                                   std::numeric_limits<std::ptrdiff_t>::max()) /
                               http3_capacity::packet_bytes;
    if (input == 0 || output == 0 || input >= max_slots || output > max_slots - input) {
        throw std::invalid_argument("HTTP/3 datagram capacities must be positive and representable");
    }
    const auto per_worker = input + output;
    // Every receive slot plus every worker's input leases and output credits.
    if (worker_count > (max_slots - http3_capacity::receive_slots) / per_worker) {
        throw std::invalid_argument("HTTP/3 aggregate datagram storage is not representable");
    }
    return {input, output, http3_capacity::receive_slots + worker_count * per_worker};
}

}  // namespace ruvia::detail
