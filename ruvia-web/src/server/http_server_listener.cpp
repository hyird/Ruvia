#include "server/http_server_listener.h"

#include <memory_resource>
#include <string>
#include <type_traits>

namespace ruvia::detail {
namespace {

http_server_listener_definition::tls clone_tls(
    const http_server_listener_definition::tls& source_value, std::pmr::memory_resource* resource) {
    http_server_listener_definition::tls result_value(source_value.certificate_, resource);
    result_value.client_certificates_ = source_value.client_certificates_;
    result_value.max_outgoing_records_ = source_value.max_outgoing_records_;
    result_value.sni_identities_.reserve(source_value.sni_identities_.size());
    for (const auto& configured : source_value.sni_identities_) {
        result_value.sni_identities_.emplace_back(configured.host_, configured.certificate_, resource);
    }
    result_value.session_ticket_keys_ = source_value.session_ticket_keys_;
    return result_value;
}

http_server_listener_definition::transport_type clone_transport(
    const http_server_listener_definition::transport_type& source_value, std::pmr::memory_resource* resource) {
    return std::visit(
        [resource]<typename transport_type>(
            const transport_type& transport) -> http_server_listener_definition::transport_type {
            if constexpr (std::is_same_v<transport_type, http_server_listener_definition::tls>) {
                return clone_tls(transport, resource);
            } else {
                return transport;
            }
        },
        source_value);
}

}  // namespace

http_server_listener_definition http_server_listener_definition::clone(std::pmr::memory_resource* resource) const {
    return http_server_listener_definition(
        endpoint_, clone_transport(transport_, pmr_resource_or_default(resource)), http3_);
}

http_server_session_config::http_server_session_config(
    const http_server_listener_definition& definition, std::pmr::memory_resource* resource)
    : transport_(clone_transport(definition.transport_, pmr_resource_or_default(resource))),
      http3_(definition.http3_),
      alternative_service_(pmr_resource_or_default(resource)),
      sni_contexts_(pmr_resource_or_default(resource)),
      quic_sni_contexts_(pmr_resource_or_default(resource)) {
    if (http3_.has_value()) {
        // RFC 7838 §3: alt-authority with an empty host names this origin's
        // host on the UDP port; "ma" bounds how long clients cache it.
        alternative_service_.append("h3=\":");
        alternative_service_.append(std::to_string(http3_->port_));
        alternative_service_.append("\"; ma=");
        alternative_service_.append(std::to_string(http3_->alt_svc_max_age_.count()));
    }
}

}  // namespace ruvia::detail
