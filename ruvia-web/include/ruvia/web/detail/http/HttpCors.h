#pragma once

#include "ruvia/web/HttpServerOptions.h"
#include "ruvia/http/HttpTypes.h"

namespace ruvia::detail {

void applyCorsHeaders(const HttpRequest& request, HttpResponse& response, const HttpServerOptions::Cors& cors);

}  // namespace ruvia::detail
