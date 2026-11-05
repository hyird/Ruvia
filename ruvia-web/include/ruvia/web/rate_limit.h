#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>

#include "ruvia/core/task.h"
#include "ruvia/web/detail/middleware/middleware_registration.h"
#include "ruvia/web/middleware.h"

namespace ruvia {

class context;

// Each worker preallocates this many sliding-window key entries when either
// the app-wide default rule or a route-specific rate-limit middleware is
// present. A key probes a bounded run of entries from its hash position; a
// key whose run holds only live entries of other keys takes the rule's
// overflow policy even when other parts of the table are free. An entry stays
// live until two of its key's windows have passed without a request.
inline constexpr std::size_t default_rate_limit_capacity_per_worker = 8192;

enum class rate_limit_overflow_policy : std::uint8_t {
    deny,
    allow,
};

// Per-worker, per-address sliding-window rule. Configuration remains a plain
// value; each owner validates it once before publishing runtime state.
// Each key's windows start at its first request and follow one another. A
// request is admitted while
//     previous * (unelapsed fraction of the current window) + current
// stays below max_requests_, where current and previous count the requests
// admitted in the current and the preceding window (a gap of two or more
// windows leaves previous at zero). The previous window's count fades out
// instead of vanishing at the boundary, so a client cannot spend one budget
// at the end of a window and a second right after it. Refused requests are
// not counted. A refusal's Retry-After is the earliest whole second at which
// the estimate admits again; X-RateLimit-Reset is when it has fallen to zero.
// Both app-wide and route rules key on the resolved client address: unscoped
// IPv6 shares a /64 budget, IPv4-mapped IPv6 shares its dotted IPv4 host budget,
// and scoped IPv6 keys on its full address plus numeric scope. Equivalent
// accepted IPv6 spellings do not create additional budgets.
struct rate_limit_rule final {
    std::size_t max_requests_{0};
    std::chrono::milliseconds window_{0};
    rate_limit_overflow_policy overflow_policy_{rate_limit_overflow_policy::deny};
};

struct rate_limit_config final {
    rate_limit_rule rule_{};
    std::size_t capacity_per_worker_{default_rate_limit_capacity_per_worker};
};
namespace detail {
// `declaration` identifies the rule's declaration: stacked rules on one route
// each count in their own window. Throws when the rule refuses the request.
void enforce_route_rate_limit(context& context_value, const void* declaration, const rate_limit_rule& rule);

}  // namespace detail

// A per-route rate limit, configured through the type:
//
//     RUVIA_GET("/ready", ready, ruvia::rate_limit<10, 1000>);
//
// Route and controller middleware lists name types, so a middleware registered
// there is default constructed and cannot take constructor arguments. Carrying
// the configuration as template parameters is how such a middleware is
// configured -- the values are constexpr, the type stays default constructible,
// and the chain is still finalized at startup with nothing allocated per
// request. A comma inside the template argument list is fine: the route macro
// pastes its arguments back together.
//
// The limit is scoped to the route it is registered on and keyed on the client
// address, so two routes carrying the same numbers count independently. Each
// declaration also counts on its own: an app or controller rule stacked over a
// route rule enforces both windows, never one shared counter. It does not
// replace application::rate_limit() -- both apply, so the stricter is what a
// caller meets, the same "narrower scope may only tighten" rule body_limit
// follows. Worker-local, like the app-wide rule, and counted with the same
// sliding-window estimate (see rate_limit_rule). A refused request is answered
// like one the app-wide rule refuses: 429 through the application error
// handler, with Retry-After and X-RateLimit-* fields.
template <std::size_t max_requests, std::int64_t window_ms,
    rate_limit_overflow_policy overflow_policy = rate_limit_overflow_policy::deny>
class rate_limit final {
public:
    static_assert(max_requests > 0, "route rate limit max requests must be greater than 0");
    static_assert(window_ms > 0, "route rate limit window must be greater than 0ms");

    static constexpr bool ruvia_uses_route_rate_limit = true;

    task<void> handle(context& context_value, next& next_value) {
        detail::enforce_route_rate_limit(context_value, this, rule_);
        co_await next_value();
    }

private:
    static constexpr rate_limit_rule rule_{
        .max_requests_ = max_requests,
        .window_ = std::chrono::milliseconds(window_ms),
        .overflow_policy_ = overflow_policy,
    };
};

}  // namespace ruvia
