#pragma once

#include <concepts>
#include <cstddef>
#include <type_traits>
#include <utility>

#include "ruvia/core/Task.h"
#include "ruvia/http/HttpResponse.h"
#include "ruvia/web/Error.h"
#include "ruvia/web/detail/Callback.h"
#include "ruvia/web/detail/CallbackRef.h"

namespace ruvia {

class Context;

namespace detail {
using HttpErrorHandlerRef = CallbackRef<Task<HttpResponse>(Context&, HttpErrorInfo)>;
using HttpNotFoundHandlerRef = CallbackRef<Task<HttpResponse>(Context&)>;
}  // namespace detail

// Answers a request that failed. Accepts a plain function -- onError(&handler)
// -- and equally a lambda or any other callable, including one that captures
// the logger, config, or metrics sink the handler needs. A self-contained
// callable is owned by the handler value; references captured by that callable
// must still outlive the registered handler.
using HttpErrorHandler = detail::Callback<Task<HttpResponse>(Context&, HttpErrorInfo)>;

// Answers a request that matched no route.
using HttpNotFoundHandler = detail::Callback<Task<HttpResponse>(Context&)>;

static_assert(sizeof(HttpErrorHandler) == 5 * sizeof(void*));
static_assert(sizeof(HttpNotFoundHandler) == 5 * sizeof(void*));

}  // namespace ruvia
