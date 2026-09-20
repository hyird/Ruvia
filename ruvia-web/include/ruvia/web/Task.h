#pragma once

#include "ruvia/core/Task.h"
#include "ruvia/http/HttpResponse.h"

namespace ruvia {

// Web and core share Task<T>; handlers spell out Task<HttpResponse> and
// operations without a result use Task<void>.

}  // namespace ruvia
