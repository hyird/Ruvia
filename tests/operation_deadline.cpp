#include <ruvia/core/detail/OperationDeadline.h>

#include <chrono>
#include <cstdint>

namespace {

enum class DeadlineKind : std::uint8_t {
    kRead,
    kWrite
};

bool operationDeadlineTransitionsAreExclusive() {
    using Deadline = ruvia::detail::OperationDeadline<DeadlineKind>;
    Deadline deadline;
    const auto now = Deadline::Clock::time_point{};
    if (deadline.kind() != nullptr || deadline.expired() || deadline.clear()) {
        return false;
    }

    deadline.arm(now + std::chrono::seconds(1), DeadlineKind::kRead);
    if (deadline.kind() == nullptr ||
        *deadline.kind() != DeadlineKind::kRead ||
        deadline.expire(now).has_value() || deadline.expired()) {
        return false;
    }

    const auto expiredKind = deadline.expire(now + std::chrono::seconds(1));
    if (expiredKind != DeadlineKind::kRead || !deadline.expired() ||
        deadline.kind() == nullptr ||
        *deadline.kind() != DeadlineKind::kRead || !deadline.clear()) {
        return false;
    }

    deadline.arm(now, DeadlineKind::kWrite);
    deadline.reset();
    return deadline.kind() == nullptr && !deadline.expired() &&
           !deadline.clear();
}

}  // namespace

int main() {
    return operationDeadlineTransitionsAreExclusive() ? 0 : 1;
}
