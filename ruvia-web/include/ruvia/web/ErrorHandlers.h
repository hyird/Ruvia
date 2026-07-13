#pragma once

#include "ruvia/core/Task.h"
#include "ruvia/http/HttpResponse.h"
#include "ruvia/web/Error.h"

namespace ruvia {

class Context;

using HttpErrorHandler = Task<HttpResponse> (*)(Context&, HttpErrorInfo);
using HttpNotFoundHandler = Task<HttpResponse> (*)(Context&);

}  // namespace ruvia
