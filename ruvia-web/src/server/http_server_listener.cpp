#include "server/http_server_listener.h"

#include <memory_resource>
#include <type_traits>

namespace ruvia::detail {
namespace {

http_server_listener_definition::tls_type clone_tls(
    const http_server_listener_definition::tls_type& source_value, std::pmr::memory_resource* resource) {
    http_server_listener_definition::tls_type result_value(source_value.certificate_, resource);
    result_value.client_certificates_ = source_value.client_certificates_;
    result_value.sni_identities_.reserve(source_value.sni_identities_.size());
    for (const auto& configured : source_value.sni_identities_) {
        result_value.sni_identities_.emplace_back(configured.host_, configured.certificate_, resource);
    }
    result_value.alt_svc_ = source_value.alt_svc_;
    result_value.http3_early_data_ = source_value.http3_early_data_;
    result_value.session_ticket_keys_ = source_value.session_ticket_keys_;
    return result_value;
}

http_server_listener_definition::transport_type clone_transport(
    const http_server_listener_definition::transport_type& source_value, std::pmr::memory_resource* resource) {
    return std::visit(
        [resource]<typename transport_type>(
            const transport_type& transport) -> http_server_listener_definition::transport_type {
            if constexpr (std::is_same_v<transport_type, http_server_listener_definition::tls_type>) {
                return clone_tls(transport, resource);
            } else {
                return transport;
            }
        },
        source_value);
}

}  // namespace

http_server_listener_definition http_server_listener_definition::clone(std::pmr::memory_resource* resource) const {
    return http_server_listener_definition(endpoint_, clone_transport(transport_, pmr_resource_or_default(resource)), http3_);
}

http_server_session_config::http_server_session_config(
    const http_server_listener_definition& definition, std::pmr::memory_resource* resource)
    : transport_(clone_transport(definition.transport_, pmr_resource_or_default(resource))),
      sni_contexts_(pmr_resource_or_default(resource)),
      sni_lookup_(pmr_resource_or_default(resource)) {}

}  // namespace ruvia::detail
