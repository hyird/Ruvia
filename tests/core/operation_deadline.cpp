#include <ruvia/core/detail/io/OperationDeadline.h>

#include <chrono>
#include <cstdint>

namespace {

enum class DeadlineKind : std::uint8_t { kRead,
    kWrite };

bool operationDeadlineTransitionsAreExclusive() {
    using Deadline = ruvia::detail::OperationDeadline<DeadlineKind>;
    Deadline deadline;
    const auto now = Deadline::Clock::time_point{};
    if (deadline.kind() != nullptr || deadline.expired() || deadline.clear()) {
        return false;
    }

    deadline.arm(now + std::chrono::seconds(1), DeadlineKind::kRead);
    if (deadline.kind() == nullptr || *deadline.kind() != DeadlineKind::kRead ||
        deadline.expire(now).has_value() || deadline.expired()) {
        return false;
    }

    const auto expiredKind = deadline.expire(now + std::chrono::seconds(1));
    if (expiredKind != DeadlineKind::kRead || !deadline.expired() || deadline.kind() == nullptr ||
        *deadline.kind() != DeadlineKind::kRead || !deadline.clear()) {
        return false;
    }

    deadline.arm(now, DeadlineKind::kWrite);
    deadline.reset();
    return deadline.kind() == nullptr && !deadline.expired() && !deadline.clear();
}

bool operationTimeoutUsesOneAbsoluteDeadline() {
    using Timeout = ruvia::detail::OperationTimeout;
    const Timeout unlimited(std::nullopt);
    if (unlimited.remaining().has_value() || unlimited.expired()) {
        return false;
    }

    const Timeout expired(std::chrono::milliseconds(0));
    if (!expired.expired() || expired.remaining() != std::chrono::milliseconds(0)) {
        return false;
    }

    const Timeout active(std::chrono::seconds(1));
    const auto remaining = active.remaining();
    return remaining.has_value() && remaining->count() > 0 && *remaining <= std::chrono::seconds(1);
}

bool positiveTimeoutRemainderDoesNotBecomeImmediate() {
    using Clock = ruvia::detail::OperationTimeout::Clock;
    const auto exact = std::chrono::duration_cast<Clock::duration>(std::chrono::milliseconds(3));
    const auto fractional = exact +
                            std::chrono::duration_cast<Clock::duration>(std::chrono::microseconds(1));
    return ruvia::detail::workerTimerCeilMilliseconds(exact) == std::chrono::milliseconds(3) &&
           ruvia::detail::workerTimerCeilMilliseconds(fractional) ==
               std::chrono::milliseconds(4) &&
           ruvia::detail::workerTimerCeilMilliseconds(Clock::duration::zero()) ==
               std::chrono::milliseconds(0);
}

}  // namespace

int main() {
    return operationDeadlineTransitionsAreExclusive() && operationTimeoutUsesOneAbsoluteDeadline() &&
                   positiveTimeoutRemainderDoesNotBecomeImmediate()
               ? 0
               : 1;
}
