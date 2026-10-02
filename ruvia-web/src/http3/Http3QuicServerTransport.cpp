#include "ruvia/web/detail/http3/Http3QuicServerTransport.h"

#include <array>
#include <stdexcept>

namespace ruvia::detail {
namespace {
constexpr std::array<unsigned char, 3> h3_alpn{2, 'h', '3'};
}

http3_quic_server_transport::http3_quic_server_transport(http3_quic_tls_context& tls,
    ruvia::quic_server_config config, std::pmr::memory_resource* resource)
    : resource_(resource != nullptr ? resource : std::pmr::get_default_resource()),
      tls_context_(tls),
      crypto_(resource_),
      sessions_(resource_) {
    server_.emplace(config, crypto_.view(), resource_);
}

http3_quic_server_transport::~http3_quic_server_transport() noexcept {
    stop_tls();
    server_.reset();
    sessions_.clear();
}

ruvia::quic_server_route http3_quic_server_transport::route_datagram(
    std::span<const std::byte> bytes, const http3_quic_datagram_address& local,
    const http3_quic_datagram_address& peer) {
    const ruvia::quic_datagram_view datagram{bytes, to_quic_address(local), to_quic_address(peer)};
    return server_->route_datagram(datagram);
}

ruvia::quic_server_admit_result http3_quic_server_transport::admit_initial(
    const ruvia::quic_initial_offer& offer, ruvia::quic_timestamp now,
    std::string_view server_name) {
    auto session = ruvia::detail::makePmrObject<tls_session>(resource_,
        tls_context_.default_context(), ruvia::quic_role::server, h3_alpn,
        server_name, resource_);
    const auto admitted = server_->admit_initial(offer, session->driver_view(), now);
    if (admitted.status != ruvia::quic_operation_status::accepted) {
        return admitted;
    }

    try {
        const auto [position, inserted] = sessions_.try_emplace(admitted.connection);
        if (!inserted) {
            throw std::logic_error("QUIC server issued a duplicate connection token");
        }
        position->second = std::move(session);
    } catch (...) {
        if (session) {
            session->stop();
        }
        (void)server_->retire(admitted.connection);
        throw;
    }
    return admitted;
}

void http3_quic_server_transport::retire(ruvia::quic_connection_token token) noexcept {
    const auto found = sessions_.find(token);
    if (found != sessions_.end()) {
        found->second->stop();
        (void)server_->retire(token);
        sessions_.erase(found);
        return;
    }
    (void)server_->retire(token);
}

void http3_quic_server_transport::stop_tls() noexcept {
    for (auto& [token, session] : sessions_) {
        (void)token;
        session->stop();
    }
}

}  // namespace ruvia::detail
