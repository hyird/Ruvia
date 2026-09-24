#pragma once

#include "ruvia/core/detail/io/OperationDeadline.h"

namespace ruvia {

// Absolute timeout shared across the asynchronous phases of one operation.
using OperationTimeout = detail::OperationTimeout;

}  // namespace ruvia
