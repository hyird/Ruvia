#include "ruvia/web/detail/http3/Http3QuicPacketIo.h"

#include "ruvia/web/detail/http3/Http3QuicDatagramBridge.h"

namespace ruvia::detail {

ruvia::quic_operation_status http3_quic_packet_io::receive(
    ruvia::quic_connection& connection, std::span<const std::byte> bytes,
    const http3_quic_datagram_address& local, const http3_quic_datagram_address& peer,
    ruvia::quic_timestamp now) {
    const ruvia::quic_datagram_view datagram{bytes, to_quic_address(local), to_quic_address(peer)};
    return connection.receive(datagram, now);
}

ruvia::quic_packet_result http3_quic_packet_io::write(
    ruvia::quic_connection& connection, std::span<std::byte> output,
    ruvia::quic_timestamp now) {
    return connection.write_packet(output, now);
}

}  // namespace ruvia::detail
