#include "ruvia/web/detail/http/context/ContextServices.h"

#include "ruvia/web/detail/server/ForwardedHeaders.h"

namespace ruvia::detail {

ConnInfo ContextServices::resolveConnInfo(const HttpRequest& request) const noexcept {
    auto resolved = connInfo_;
    // Fail closed: with no trusted set configured, or a peer outside it, the
    // forwarding headers are never even read. They are attacker-controlled
    // otherwise, and believing them would let any caller choose its own
    // rate-limit key and claim a secure scheme.
    if (trustedProxies_ == nullptr || !trustedProxies_->trusts(resolved.remote().address())) {
        return resolved;
    }
    const auto forwarded = resolveForwardedClient(request);
    resolved.applyForwarded(forwarded.address, forwarded.scheme);
    return resolved;
}

}  // namespace ruvia::detail
