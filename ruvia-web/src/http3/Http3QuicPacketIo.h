#pragma once

#include <cstddef>
#include <span>

#include "ruvia/http/quic_connection.h"

#include "http3/Http3QuicDatagramBridge.h"

namespace ruvia::detail {

// Thin UDP/protocol boundary. Datagram and packet storage belongs to the caller;
// this adapter never retains a borrowed network buffer.
class http3_quic_packet_io final {
public:
    [[nodiscard]] static ruvia::quic_operation_status receive(
        ruvia::quic_connection& connection, std::span<const std::byte> bytes,
        const http3_quic_datagram_address& local, const http3_quic_datagram_address& peer,
        ruvia::quic_timestamp now);

    [[nodiscard]] static ruvia::quic_packet_result write(
        ruvia::quic_connection& connection, std::span<std::byte> output,
        ruvia::quic_timestamp now);
};

}  // namespace ruvia::detail
