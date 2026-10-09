#include "ruvia/core/operation_deadline.h"

#include <chrono>
#include <cstdint>

#include "ruvia/core/operation_timeout.h"
#include "ruvia/core/worker_timer.h"

namespace {

enum class deadline_kind : std::uint8_t { read,
    write };

bool operation_deadline_transitions_are_exclusive() {
    using deadline_type = ruvia::operation_deadline<deadline_kind>;
    deadline_type deadline;
    const auto now = deadline_type::clock::time_point{};
    if (deadline.kind() != nullptr || deadline.expired() || deadline.clear()) {
        return false;
    }

    deadline.arm(now + std::chrono::seconds(1), deadline_kind::read);
    if (deadline.kind() == nullptr || *deadline.kind() != deadline_kind::read ||
        deadline.expire(now).has_value() || deadline.expired()) {
        return false;
    }

    const auto expired_kind = deadline.expire(now + std::chrono::seconds(1));
    if (expired_kind != deadline_kind::read || !deadline.expired() || deadline.kind() == nullptr ||
        *deadline.kind() != deadline_kind::read || !deadline.clear()) {
        return false;
    }

    deadline.arm(now, deadline_kind::write);
    deadline.reset();
    return deadline.kind() == nullptr && !deadline.expired() && !deadline.clear();
}

bool operation_timeout_uses_one_absolute_deadline() {
    using timeout_type = ruvia::operation_timeout;
    const timeout_type unlimited(std::nullopt);
    if (unlimited.deadline().has_value() || unlimited.remaining().has_value() ||
        unlimited.expired()) {
        return false;
    }

    const timeout_type expired(std::chrono::milliseconds(0));
    if (!expired.deadline().has_value() || !expired.expired() ||
        expired.remaining() != std::chrono::milliseconds(0)) {
        return false;
    }

    const timeout_type active(std::chrono::seconds(1));
    const auto deadline_value = active.deadline();
    const auto remaining = active.remaining();
    if (!deadline_value.has_value() || !remaining.has_value() || remaining->count() <= 0 ||
        *remaining > std::chrono::seconds(1)) {
        return false;
    }
    return active.constrained_by(std::chrono::seconds(2)).deadline() == deadline_value &&
           unlimited.constrained_by(std::chrono::seconds(2)).deadline().has_value();
}

bool positive_timeout_remainder_does_not_become_immediate() {
    using clock_type = ruvia::operation_timeout::clock_type;
    const auto exact = std::chrono::duration_cast<clock_type::duration>(std::chrono::milliseconds(3));
    const auto fractional = exact +
                            std::chrono::duration_cast<clock_type::duration>(std::chrono::microseconds(1));
    return ruvia::worker_timer_ceil_milliseconds(exact) == std::chrono::milliseconds(3) &&
           ruvia::worker_timer_ceil_milliseconds(fractional) ==
               std::chrono::milliseconds(4) &&
           ruvia::worker_timer_ceil_milliseconds(clock_type::duration::zero()) ==
               std::chrono::milliseconds(0);
}

}  // namespace

int main() {
    return operation_deadline_transitions_are_exclusive() && operation_timeout_uses_one_absolute_deadline() &&
                   positive_timeout_remainder_does_not_become_immediate()
               ? 0
               : 1;
}
