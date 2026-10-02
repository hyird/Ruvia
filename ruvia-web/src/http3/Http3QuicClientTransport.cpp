#include "ruvia/web/detail/http3/Http3QuicClientTransport.h"

#include <array>
#include <stdexcept>

namespace ruvia::detail {
namespace {
constexpr std::array<unsigned char, 3> h3_alpn{2, 'h', '3'};
}

http3_quic_client_transport::http3_quic_client_transport(http3_quic_client_tls_context& tls,
    ruvia::quic_connection_config config, std::string_view host,
    ruvia::quic_timestamp now, std::pmr::memory_resource* resource)
    : resource_(resource != nullptr ? resource : std::pmr::get_default_resource()),
      crypto_(resource_),
      tls_session_(tls.native_handle(), ruvia::quic_role::client, h3_alpn, host, resource_) {
    tls.prepare(tls_session_.native_handle(), host);
    config.role = ruvia::quic_role::client;
    auto provider = crypto_.view();
    if (config.destination_connection_id.size() == 0) {
        std::array<std::byte, 8> destination_id{};
        provider.random_bytes(provider.context, destination_id);
        config.destination_connection_id = ruvia::quic_connection_id(destination_id);
    }
    connection_.emplace(config, provider, tls_session_.driver_view(), resource_, now);
}

http3_quic_client_transport::~http3_quic_client_transport() noexcept {
    stop_tls();
    connection_.reset();
}

ruvia::quic_operation_status http3_quic_client_transport::receive(
    const ruvia::quic_datagram_view& datagram, ruvia::quic_timestamp now) {
    return connection_->receive(datagram, now);
}

ruvia::quic_packet_result http3_quic_client_transport::write_packet(
    std::span<std::byte> output, ruvia::quic_timestamp now) {
    return connection_->write_packet(output, now);
}

std::optional<ruvia::quic_timestamp> http3_quic_client_transport::next_expiry() const noexcept {
    return connection_->next_expiry();
}

ruvia::quic_operation_status http3_quic_client_transport::handle_expiry(ruvia::quic_timestamp now) {
    return connection_->handle_expiry(now);
}

void http3_quic_client_transport::stop_tls() noexcept {
    tls_session_.stop();
}

void http3_quic_client_transport::close() noexcept {
    stop_tls();
    connection_.reset();
}

}  // namespace ruvia::detail
