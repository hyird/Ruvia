#pragma once

#include "ruvia/http/http_request.h"
#include "ruvia/http/http_response.h"

#include "http/cors_options.h"

namespace ruvia::detail {

void apply_cors_headers(const http_request& request, http_response& response, const cors_options& cors);

}  // namespace ruvia::detail
