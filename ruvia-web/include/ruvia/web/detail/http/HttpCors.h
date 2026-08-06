#pragma once

#include "ruvia/http/HttpRequest.h"
#include "ruvia/http/HttpResponse.h"
#include "ruvia/web/detail/http/CorsOptions.h"

namespace ruvia::detail {

void applyCorsHeaders(const HttpRequest& request, HttpResponse& response, const CorsOptions& cors);

}  // namespace ruvia::detail
