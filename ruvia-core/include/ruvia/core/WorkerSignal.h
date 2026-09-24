#pragma once

#include "ruvia/core/detail/worker/WorkerSignal.h"

namespace ruvia {

// Allocation-free worker-affine wake primitive.
class WorkerSignal final : public detail::WorkerSignal {
public:
    using detail::WorkerSignal::WorkerSignal;
};

}  // namespace ruvia
