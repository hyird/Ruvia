#pragma once

#include "ruvia/core/detail/worker/WorkerRuntimeContext.h"

namespace ruvia {

// Owns the worker dispatcher endpoint and detaches escaped handles on teardown.
class WorkerRuntimeContext final : public detail::WorkerRuntimeContext {
public:
    using detail::WorkerRuntimeContext::WorkerRuntimeContext;
};

}  // namespace ruvia
