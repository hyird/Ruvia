#include "http3/http3_quic_server_transport.h"

#include <stdexcept>

namespace ruvia::detail {

http3_quic_server_transport::http3_quic_server_transport(http3_quic_tls_context& tls,
    ruvia::quic_server_config config, std::pmr::memory_resource* resource)
    : resource_(resource != nullptr ? resource : std::pmr::get_default_resource()),
      tls_context_(tls),
      crypto_(resource_),
      sessions_(resource_) {
    sessions_.reserve(config.max_active_connections_);
    server_.emplace(config, crypto_.view(), resource_);
}

http3_quic_server_transport::~http3_quic_server_transport() noexcept {
    stop_tls();
    server_.reset();
    sessions_.clear();
}

ruvia::quic_server_admit_result http3_quic_server_transport::admit_initial(
    const ruvia::quic_initial_offer& offer, ruvia::quic_timestamp now,
    std::string_view server_name) {
    // The caller drops an offer whose admission throws, so a failure before the
    // HTTP server sees it must release the pending Initial too.
    auto session_value = [&] {
        try {
            return ruvia::make_pmr_object<tls_session>(resource_,
                tls_context_.default_context(), ruvia::quic_role::server,
                server_name, resource_, nullptr, tls_context_.early_data_enabled());
        } catch (...) {
            (void)server_->discard_initial(offer);
            throw;
        }
    }();
    const auto admitted = server_->admit_initial(offer, session_value->driver_view(), now);
    if (admitted.status_ != ruvia::quic_operation_status::accepted) {
        return admitted;
    }

    try {
        const auto [position, inserted] = sessions_.try_emplace(admitted.connection_);
        if (!inserted) {
            throw std::logic_error("QUIC server issued a duplicate connection token");
        }
        position->second = std::move(session_value);
    } catch (...) {
        if (session_value) {
            session_value->stop();
        }
        (void)server_->retire(admitted.connection_);
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
