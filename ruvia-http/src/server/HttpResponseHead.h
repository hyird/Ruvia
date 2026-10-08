#pragma once

#include "ruvia/http/Http1ResponseHeadPlan.h"

#include "server/HttpResponseHeadBuffer.h"

namespace ruvia {

class HttpResponse;

namespace detail {

void appendResponseHead(
    const HttpResponse& response, ResponseHeadBuffer& head, const Http1ResponseHeadPlan& plan);

}  // namespace detail
}  // namespace ruvia
