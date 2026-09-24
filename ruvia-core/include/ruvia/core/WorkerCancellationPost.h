#pragma once

#include "ruvia/core/detail/worker/WorkerCancellationPost.h"

namespace ruvia {
template <typename Owner>
using WorkerCancellationMailbox = detail::WorkerCancellationMailbox<Owner>;
template <typename Owner>
using WorkerCancellationDispatch = detail::WorkerCancellationDispatch<Owner>;
template <typename Mailbox>
using WorkerCancellationPost = detail::WorkerCancellationPost<Mailbox>;
using detail::makeWorkerCancellationMailbox;
template <typename Mailbox>
inline constexpr bool workerCancellationPostIsInline = detail::workerCancellationPostIsInline<Mailbox>;
}
