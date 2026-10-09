#include "context/context_services.h"
#include "server/forwarded_headers.h"

namespace ruvia::detail {

conn_info context_services::resolve_conn_info(const http_request& request) const noexcept {
    auto resolved = connection_services_.info_;
    // Fail closed: with no trusted set configured, or a peer outside it, the
    // forwarding headers are never even read. They are attacker-controlled
    // otherwise, and believing them would let any caller choose its own
    // rate-limit key and claim a secure scheme.
    if (worker_services_.trusted_proxies_ == nullptr || !worker_services_.trusted_proxies_->trusts(resolved.remote().address())) {
        return resolved;
    }
    const auto forwarded = resolve_forwarded_client(request, *worker_services_.trusted_proxies_);
    resolved.apply_forwarded(forwarded.address_, forwarded.scheme_);
    return resolved;
}

}  // namespace ruvia::detail
