#pragma once

#include "ruvia/http/detail/HeaderAcceptUtils.h"
#include "ruvia/http/detail/server/HttpResponseWritePlan.h"
#include "ruvia/web/HttpServerOptions.h"
#include "ruvia/http/HttpTypes.h"

namespace ruvia::detail {

void applyResponseCompression(
    HttpContentCoding coding,
    HttpKnownMethod requestMethod,
    HttpResponse& response,
    const HttpServerOptions::Compression& options);

}  // namespace ruvia::detail
