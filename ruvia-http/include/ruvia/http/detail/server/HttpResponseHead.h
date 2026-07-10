#pragma once

#include "ruvia/http/detail/server/HttpResponseHeadBuffer.h"
#include "ruvia/http/detail/server/HttpResponseHeadPolicy.h"

namespace ruvia {

class HttpResponse;

namespace detail {

void appendResponseHead(
    const HttpResponse& response,
    ResponseHeadBuffer& head,
    ResponseWritePolicy policy,
    bool suppressAutoContentLength = false);

}  // namespace detail
}  // namespace ruvia
