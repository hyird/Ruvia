#pragma once

#include "ruvia/core/Task.h"
#include "ruvia/http/HttpResponse.h"
#include "ruvia/web/ErrorHandlers.h"

#include <memory_resource>

namespace ruvia::detail {

[[nodiscard]] HttpResponse makeDefaultErrorResponse(
    std::pmr::memory_resource* resource, HttpErrorInfo error);

[[nodiscard]] Task<HttpResponse> invokeErrorHandler(
    Context& context, HttpErrorInfo error, HttpErrorHandlerRef handler);

}  // namespace ruvia::detail
