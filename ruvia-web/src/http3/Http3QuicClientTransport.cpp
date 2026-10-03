#include "ruvia/web/detail/http3/Http3QuicClientTransport.h"

#include <array>
#include <stdexcept>

#include <openssl/ssl.h>

namespace ruvia::detail {
namespace {
constexpr std::array<unsigned char, 3> h3_alpn{2, 'h', '3'};
}

http3_quic_client_transport::http3_quic_client_transport(http3_quic_client_tls_context& tls,
    ruvia::quic_connection_config config, std::string_view host,
    ruvia::quic_timestamp now, std::pmr::memory_resource* resource,
    bool enable_early_data)
    : resource_(resource != nullptr ? resource : std::pmr::get_default_resource()),
      tls_(&tls),
      host_(host, resource_),
      crypto_(resource_) {
    auto ticket = tls.take_ticket(host, config.version);
    early_data_enabled_ = enable_early_data && ticket && ticket->session &&
                          SSL_SESSION_get_max_early_data(ticket->session.get()) != 0 &&
                          ticket->settings.has_value() && !ticket->transport_parameters.empty();
    tls_session_.emplace(tls.native_handle(), ruvia::quic_role::client, h3_alpn, host,
        resource_, ticket ? ticket->session.get() : nullptr, early_data_enabled_);
    tls.prepare(tls_session_->native_handle(), host);
    config.role = ruvia::quic_role::client;
    auto provider = crypto_.view();
    if (config.destination_connection_id.size() == 0) {
        std::array<std::byte, 8> destination_id{};
        provider.random_bytes(provider.context, destination_id);
        config.destination_connection_id = ruvia::quic_connection_id(destination_id);
    }
    connection_.emplace(config, provider, tls_session_->driver_view(), resource_, now,
        early_data_enabled_ ? std::span<const std::byte>(ticket->transport_parameters)
                            : std::span<const std::byte>{});
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

void http3_quic_client_transport::remember_resumption_ticket(
    std::optional<ruvia::Http3Settings> settings) {
    if (!connection_ || !tls_session_ || !connection_->info().tls_handshake_complete) {
        return;
    }
    auto session = tls_session_->take_resumption_session();
    if (!session) {
        return;
    }
    std::array<std::byte, 4096> encoded{};
    const auto size = connection_->encode_early_transport_parameters(encoded);
    tls_->remember_ticket(session.get(), host_, connection_->info().negotiated_version,
        std::span<const std::byte>(encoded).first(size), std::move(settings));
}

void http3_quic_client_transport::stop_tls() noexcept {
    if (tls_session_) {
        tls_session_->stop();
    }
}

void http3_quic_client_transport::close() noexcept {
    stop_tls();
    connection_.reset();
}

}  // namespace ruvia::detail
