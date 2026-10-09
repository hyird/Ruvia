#pragma once

#include "ruvia/http/http1_server_semantics.h"
#include "ruvia/http/http_response.h"

#include "server/http1_request_sequence.h"

namespace ruvia::detail {

[[nodiscard]] inline http1_request_connection_plan require_http1_final_response_commit(
    http_response& response, http1_request_connection_plan connection_plan) {
    const auto result_value = http1_commit_final_response(response, connection_plan);
    if (const auto* failure = result_value.failure()) {
        throw failure->exception();
    }
    return *result_value.committed();
}

[[nodiscard]] inline http1_request_connection_plan finalize_buffered_route_response(http_response& response,
    http1_request_connection_plan connection_plan, http1_request_sequence& request_sequence) {
    connection_plan = request_sequence.complete_uncommitted_response(connection_plan);
    return require_http1_final_response_commit(response, connection_plan);
}

[[nodiscard]] inline http1_request_connection_plan finalize_body_route_response(http_response& response,
    http1_request_connection_plan connection_plan, http1_request_sequence& request_sequence,
    http1_request_body_consumption body_consumption) {
    connection_plan = apply_request_body_consumption(connection_plan, body_consumption);
    connection_plan = request_sequence.complete_uncommitted_response(connection_plan);
    // Fix borrowed response views before callers restore pipeline bytes.
    response.materialize_body();
    return require_http1_final_response_commit(response, connection_plan);
}

}  // namespace ruvia::detail
