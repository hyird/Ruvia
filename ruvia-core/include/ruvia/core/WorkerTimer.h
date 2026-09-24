#pragma once

#include "ruvia/core/detail/worker/WorkerTimer.h"

namespace ruvia {
using WorkerTimerCancellation = detail::WorkerTimerCancellation;
using WorkerTimerOutcome = detail::WorkerTimerOutcome;
using WorkerTimerRegistration = detail::WorkerTimerRegistration;
using detail::workerTimerDeadlineAfter;
using detail::workerTimerSaturatingDeadline;
using detail::workerTimerSaturatingDurationCast;
}
