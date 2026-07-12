#pragma once

#include "ruvia/http/HttpRequest.h"
#include "ruvia/http/HttpResponse.h"
#include "ruvia/web/HttpServerOptions.h"

namespace ruvia::detail {

void applyCorsHeaders(const HttpRequest& request, HttpResponse& response, const HttpServerOptions::Cors& cors);

}  // namespace ruvia::detail
