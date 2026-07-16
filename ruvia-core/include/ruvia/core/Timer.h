#pragma once

#include <chrono>

#include <ruvia/core/Task.h>
#include <ruvia/core/WorkerHandle.h>

namespace ruvia {

[[nodiscard]] Task<void>
sleepFor(WorkerHandle worker, std::chrono::steady_clock::duration duration);

}
