#include "server/http_server_listener.h"

#include <memory_resource>
#include <optional>
#include <string>
#include <type_traits>

namespace ruvia::detail {
namespace {

// The identity set is immutable and shared, not copied.
http_server_listener_definition::tls clone_tls(
    const http_server_listener_definition::tls& source_value, std::pmr::memory_resource* resource) {
    http_server_listener_definition::tls result_value(source_value.identities_);
    if (const auto& policy = source_value.client_certificates_; policy.has_value()) {
        result_value.client_certificates_.emplace(http_server_listener_definition::tls_client_certificate_policy{
            .requirement_ = policy->requirement_,
            .client_ca_ = policy->client_ca_,
            .ca_file_ = tls_file_name(policy->ca_file_, resource),
        });
    }
    result_value.max_outgoing_records_ = source_value.max_outgoing_records_;
    result_value.session_ticket_keys_ = source_value.session_ticket_keys_;
    result_value.tcp_mode_ = source_value.tcp_mode_;
    result_value.plaintext_redirect_ = source_value.plaintext_redirect_;
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

std::optional<http_server_listener_definition::connection_dispatch> clone_connection_dispatch(
    const std::optional<http_server_listener_definition::connection_dispatch>& source_value,
    std::pmr::memory_resource* resource) {
    std::optional<http_server_listener_definition::connection_dispatch> result_value;
    if (source_value.has_value()) {
        result_value.emplace(source_value->alpn_, resource);
    }
    return result_value;
}

}  // namespace

http_server_listener_definition http_server_listener_definition::clone(std::pmr::memory_resource* resource) const {
    http_server_listener_definition result_value(
        endpoint_, clone_transport(transport_, pmr_resource_or_default(resource)), http3_);
    result_value.connection_ = clone_connection_dispatch(connection_, resource);
    result_value.inherited_sockets_ = inherited_sockets_;
    result_value.ipv6_only_ = ipv6_only_;
    result_value.request_stream_ = request_stream_;
    return result_value;
}

http_server_session_config::http_server_session_config(
    const http_server_listener_definition& definition, std::pmr::memory_resource* resource)
    : transport_(clone_transport(definition.transport_, pmr_resource_or_default(resource))),
      ipv6_only_(definition.ipv6_only_),
      http3_(definition.http3_),
      connection_(clone_connection_dispatch(definition.connection_, resource)),
      request_stream_(definition.request_stream_),
      alternative_service_(pmr_resource_or_default(resource)) {
    if (http3_.has_value()) {
        // RFC 7838 §3: alt-authority with an empty host names this origin's
        // host on the UDP port; "ma" bounds how long clients cache it.
        alternative_service_.append("h3=\":");
        alternative_service_.append(std::to_string(http3_->port_));
        alternative_service_.append("\"; ma=");
        alternative_service_.append(std::to_string(http3_->alt_svc_max_age_.count()));
    }
}

void http_server_session_config::adopt_tls_identities(
    std::shared_ptr<const http_server_tls_identities> identities) noexcept {
    if (auto* tls = std::get_if<http_server_listener_definition::tls>(&transport_)) {
        tls->identities_.swap(identities);
    }
}

}  // namespace ruvia::detail
