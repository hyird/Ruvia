#pragma once

#include "net/server/HttpResponseHeadBuffer.h"
#include "net/server/HttpResponseHeadPolicy.h"

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
