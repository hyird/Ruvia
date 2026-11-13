#pragma once

#include "ruvia/core/task.h"
#include "ruvia/http/http_response.h"
#include "ruvia/web/error.h"

namespace ruvia::detail {

[[nodiscard]] http_response make_default_error_response(http_error_info error);

// The default 404 answer for a request no route matched.
[[nodiscard]] http_response make_route_not_found_response();

[[nodiscard]] task<http_response> invoke_error_handler(
    context& context_value, http_error_info error, http_error_handler_ref handler);

}  // namespace ruvia::detail
