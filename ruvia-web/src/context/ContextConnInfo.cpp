#include "context/ContextServices.h"
#include "server/ForwardedHeaders.h"

namespace ruvia::detail {

ConnInfo ContextServices::resolveConnInfo(const HttpRequest& request) const noexcept {
    auto resolved = connection_services_.info;
    // Fail closed: with no trusted set configured, or a peer outside it, the
    // forwarding headers are never even read. They are attacker-controlled
    // otherwise, and believing them would let any caller choose its own
    // rate-limit key and claim a secure scheme.
    if (worker_services_.trusted_proxies == nullptr || !worker_services_.trusted_proxies->trusts(resolved.remote().address())) {
        return resolved;
    }
    const auto forwarded = resolveForwardedClient(request, *worker_services_.trusted_proxies);
    resolved.applyForwarded(forwarded.address, forwarded.scheme);
    return resolved;
}

}  // namespace ruvia::detail
