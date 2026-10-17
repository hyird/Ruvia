#pragma once

#include <cstddef>

namespace ruvia::detail {

// Per-worker datagram storage. Every worker owns one socket bound to the
// shared listen endpoint and one packet pool sized for that socket's native
// operations; nothing is shared across workers.
struct http3_capacity final {
    // One pool slot holds one UDP payload. 1472 bytes is the largest payload of
    // a 1500-byte Ethernet MTU: it covers peers' padded Initials above the
    // 1200-byte minimum and every packet the server transmits, which never
    // exceeds its advertised max_udp_payload_size. A larger datagram arrives
    // truncated and is dropped like any other malformed packet.
    static constexpr std::size_t packet_bytes = 1472;
    // Receive leases the socket keeps armed or holding packets not yet consumed.
    static constexpr std::size_t receive_slots = 32;
    // Native sends in flight at once; each holds its pool slot until completion.
    static constexpr std::size_t send_slots = 64;
    // One slot stays available for the packet reservation under construction.
    static constexpr std::size_t pool_slots = receive_slots + send_slots + 1;
};

}  // namespace ruvia::detail
