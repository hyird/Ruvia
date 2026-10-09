#pragma once

#include "ruvia/core/task.h"
#include "ruvia/http/http_response.h"

namespace ruvia {

// Web and core share task<T>; handlers spell out task<http_response> and
// operations without a result use task<void>.

}  // namespace ruvia
