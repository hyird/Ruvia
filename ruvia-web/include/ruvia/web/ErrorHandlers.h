#pragma once

#include "ruvia/core/Task.h"
#include "ruvia/http/Error.h"

namespace ruvia {

class Context;

using HttpErrorHandler = Task<HttpResponse> (*)(Context&, HttpErrorInfo);
using HttpNotFoundHandler = Task<HttpResponse> (*)(Context&);

[[nodiscard]] Task<HttpResponse> makeErrorResponse(
    Context& context,
    HttpErrorInfo error,
    bool closeConnection,
    HttpErrorHandler handler);

}  // namespace ruvia
