#pragma once

#include "ruvia/http/HttpKnownMethod.h"
#include "ruvia/http/detail/coding/HttpContentCoding.h"
#include "ruvia/http/detail/server/HttpResponseWritePlan.h"
#include "ruvia/web/ServerConfig.h"
#include "ruvia/http/HttpResponse.h"

namespace ruvia::detail {

void applyResponseCompression(
    HttpContentCoding coding,
    HttpKnownMethod requestMethod,
    HttpResponse& response,
    const CompressionConfig& options);

}  // namespace ruvia::detail
