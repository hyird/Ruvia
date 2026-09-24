#pragma once

#include "ruvia/core/WorkerTimer.h"

namespace ruvia::detail {
using WorkerTimerCancellation = ::ruvia::WorkerTimerCancellation;
using WorkerTimerOutcome = ::ruvia::WorkerTimerOutcome;
using WorkerTimerRegistration = ::ruvia::WorkerTimerRegistration;
using ::ruvia::workerTimerCeilMilliseconds;
using ::ruvia::workerTimerDeadlineAfter;
using ::ruvia::workerTimerSaturatingDeadline;
using ::ruvia::workerTimerSaturatingDurationCast;
}  // namespace ruvia::detail
