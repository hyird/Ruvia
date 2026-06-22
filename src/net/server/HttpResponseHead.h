#pragma once

#include "HttpResponseHeadBuffer.h"
#include "HttpResponseHeadPolicy.h"

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
