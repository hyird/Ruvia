#pragma once

#include "ruvia/core/Task.h"
#include "ruvia/http/HttpResponse.h"

namespace ruvia {

// Web handlers return HTTP responses by default. Core-only code spells out
// its result type, including Task<void> for operations without a result.
template <typename T = HttpResponse>
class Task;

}  // namespace ruvia
