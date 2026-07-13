#pragma once

#include "ruvia/http/detail/http1/Http1ResponseHeadPlan.h"
#include "ruvia/http/detail/server/HttpResponseHeadBuffer.h"

namespace ruvia {

class HttpResponse;

namespace detail {

void appendResponseHead(
    const HttpResponse& response,
    ResponseHeadBuffer& head,
    const Http1ResponseHeadPlan& plan);

}  // namespace detail
}  // namespace ruvia
