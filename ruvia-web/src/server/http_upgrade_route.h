#pragma once

#include <exception>
#include <optional>
#include <utility>

#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/core/task.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_response.h"
#include "ruvia/web/detail/callback_ref.h"

#include "context/context_services.h"
#include "router/route_resolution.h"
#include "router/route_table.h"

// The shared shape of every upgrade-capable route (CONNECT / CONNECT-UDP
// tunnels and WebSocket upgrades) over HTTP/1: run the
// application chain with a terminal that commits the upgrade and
// owns the resulting session; once a session exists it is always finished,
// otherwise a handler failure propagates and an uncommitted response falls
// back to the ordinary buffered tail.

namespace ruvia::detail {

// `establish(context&) -> task<void>` commits the upgrade response head,
// emplaces `session` and runs the route's stream handler on it.
// `finish(session_type&, std::exception_ptr) -> task<void>` closes an
// established session and reports a handler failure; it always runs once a
// session exists, before any handler exception is surfaced.
// Returns a buffered response only when the application answered without
// upgrading; an established or abandoned upgrade returns std::nullopt.
template <typename session_type, typename establish_type, typename finish_type>
[[nodiscard]] task<std::optional<http_response>> run_http_upgrade_route(
    const route_table& routes, const http_request& request, const resolved_route& resolved, request_memory& memory,
    context_services services, std::optional<session_type>& session, establish_type& establish,
    finish_type& finish) {
    const auto terminal = callback_access::bind<task<void>(context&)>(establish);
    std::optional<http_response> buffered;
    std::exception_ptr failure;
    try {
        buffered = co_await routes.dispatch_upgrade(request, resolved, memory, terminal, std::move(services));
    } catch (...) {
        failure = std::current_exception();
    }
    if (session.has_value()) {
        co_await finish(*session, failure);
        co_return std::nullopt;
    }
    if (failure != nullptr) {
        std::rethrow_exception(failure);
    }
    co_return buffered;
}

}  // namespace ruvia::detail
