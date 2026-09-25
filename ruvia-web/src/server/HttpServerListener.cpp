#include "ruvia/web/detail/server/HttpServerListener.h"

#include <memory_resource>
#include <type_traits>

namespace ruvia::detail {
namespace {

HttpServerListenerDefinition::TlsIdentity cloneTlsIdentity(
    const HttpServerListenerDefinition::TlsIdentity& source, std::pmr::memory_resource* resource) {
    HttpServerListenerDefinition::TlsIdentity result(ResolvedPmrResourceTag{}, resource);
    result.certificateChainFile = source.certificateChainFile;
    result.privateKeyFile = source.privateKeyFile;
    result.privateKeyPassword = source.privateKeyPassword;
    return result;
}

HttpServerListenerDefinition::Tls cloneTls(
    const HttpServerListenerDefinition::Tls& source, std::pmr::memory_resource* resource) {
    HttpServerListenerDefinition::Tls result(ResolvedPmrResourceTag{}, resource);
    result.identity = cloneTlsIdentity(source.identity, resource);
    if (source.clientCertificates.has_value()) {
        auto& policy = result.clientCertificates.emplace(
            ResolvedPmrResourceTag{}, resource, source.clientCertificates->requirement);
        policy.verifyFile = source.clientCertificates->verifyFile;
    }
    result.sniIdentities.reserve(source.sniIdentities.size());
    for (const auto& configured : source.sniIdentities) {
        auto& sni = result.sniIdentities.emplace_back(ResolvedPmrResourceTag{}, resource);
        sni.host = configured.host;
        sni.identity = cloneTlsIdentity(configured.identity, resource);
    }
    return result;
}

HttpServerListenerDefinition::Transport cloneTransport(
    const HttpServerListenerDefinition::Transport& source, std::pmr::memory_resource* resource) {
    return std::visit(
        [resource]<typename Transport>(
            const Transport& transport) -> HttpServerListenerDefinition::Transport {
            if constexpr (std::is_same_v<Transport, HttpServerListenerDefinition::Tls>) {
                return cloneTls(transport, resource);
            } else {
                return transport;
            }
        },
        source);
}

}  // namespace

HttpServerSessionConfig::HttpServerSessionConfig(
    const HttpServerListenerDefinition& definition, std::pmr::memory_resource* resource)
    : transport(cloneTransport(definition.transport, pmrResourceOrDefault(resource))),
      sniContexts(pmrResourceOrDefault(resource)),
      sniLookup(pmrResourceOrDefault(resource)) {}

HttpServerAcceptor::HttpServerAcceptor(asio::io_context& ioContext,
    const HttpServerListenerDefinition& definition)
    : acceptor(ioContext), endpoint(definition.endpoint) {}

}  // namespace ruvia::detail
