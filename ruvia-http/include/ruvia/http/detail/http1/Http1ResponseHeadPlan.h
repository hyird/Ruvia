#pragma once

#include "ruvia/http/Http1ResponseHeadPlan.h"
#include "ruvia/http/Http1ServerConnectionPlan.h"

namespace ruvia::detail {
using ::ruvia::Http1ResponseHeadPlan;
using ::ruvia::Http1ServerConnectionPlan;
using ::ruvia::http1PlanHttp10RequestConnection;
using ::ruvia::http1PlanHttp11RequestConnection;
using ::ruvia::Http1BufferedResponsePlan;
using ::ruvia::http1BufferedResponsePlan;
using ::ruvia::http1KnownLengthResponseStreamHeadPlan;
using ::ruvia::http1ChunkedResponseStreamHeadPlan;
using ::ruvia::http1CloseDelimitedResponseStreamHeadPlan;
}
