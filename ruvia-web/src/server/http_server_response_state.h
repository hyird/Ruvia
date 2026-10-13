#pragma once

#include <cstddef>

#include "ruvia/http/http1_server_semantics.h"
#include "ruvia/http/http_response.h"

#include "server/http1_request_sequence.h"
#include "server/http1_session_request_completion.h"

namespace ruvia::detail {

// Commits a response the framework produced (an error or exception handler's
// output). Failing to commit it leaves no response to fall back to.
[[nodiscard]] inline http1_request_connection_plan require_http1_final_response_commit(
    http_response& response, http1_request_connection_plan connection_plan) {
    const auto result_value = http1_commit_final_response(response, connection_plan);
    if (const auto* failure = result_value.failure()) {
        throw failure->exception();
    }
    return *result_value.committed();
}

// Commits a framework response that ends the connection: a rejection, a
// failed route, or a route response the connection cannot outlive.
[[nodiscard]] inline http1_session_request_completion close_with_framework_response(
    http_response& response, http1_request_connection_plan connection_plan) {
    return http1_session_request_completion::make_buffered_closing(
        require_http1_final_response_commit(response, connection_plan.require_close()));
}

// Commits a route's buffered response. A response that breaks the HTTP/1
// final-response rules becomes a rejected completion that the session answers
// through the exception handler instead of the route response.
template <typename make_completion_type>
[[nodiscard]] inline http1_session_request_completion commit_http1_route_response(http_response& response,
    http1_request_connection_plan connection_plan, make_completion_type make_completion) {
    const auto result_value = http1_commit_final_response(response, connection_plan);
    if (const auto* failure = result_value.failure()) {
        return http1_session_request_completion::make_rejected_final_response(connection_plan, *failure);
    }
    return make_completion(*result_value.committed());
}

[[nodiscard]] inline http1_session_request_completion finalize_buffered_route_response(http_response& response,
    http1_request_connection_plan connection_plan, http1_request_sequence& request_sequence,
    std::size_t consumed_bytes) {
    connection_plan = request_sequence.complete_uncommitted_response(connection_plan);
    return commit_http1_route_response(response, connection_plan,
        [consumed_bytes](http1_request_connection_plan committed) {
            return http1_session_request_completion::make_buffered_unrestored(committed, consumed_bytes);
        });
}

// Route responses for an upgrade request always close the connection.
[[nodiscard]] inline http1_session_request_completion finalize_closing_route_response(
    http_response& response, http1_request_connection_plan connection_plan) {
    return commit_http1_route_response(response, connection_plan.require_close(),
        [](http1_request_connection_plan committed) {
            return http1_session_request_completion::make_buffered_closing(committed);
        });
}

}  // namespace ruvia::detail
