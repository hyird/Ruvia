#include <gtest/gtest.h>

#include <chrono>
#include <concepts>
#include <cstdint>
#include <limits>
#include <ratio>
#include <stdexcept>
#include <type_traits>

#include "ruvia/core/operation_timeout.h"

namespace {

template <typename left_type, typename right_type>
concept timeout_orderable = requires(left_type left, right_type right) { left < right; };

template <typename left_type, typename right_type>
concept timeout_equatable = requires(left_type left, right_type right) { left == right; };

static_assert(std::totally_ordered<ruvia::timeout_duration>);
static_assert(!timeout_orderable<ruvia::timeout_duration, std::chrono::nanoseconds>);
static_assert(!timeout_orderable<std::chrono::nanoseconds, ruvia::timeout_duration>);
static_assert(!timeout_equatable<ruvia::timeout_duration, std::chrono::nanoseconds>);
static_assert(!timeout_equatable<std::chrono::nanoseconds, ruvia::timeout_duration>);

enum class deadline_kind : std::uint8_t { read,
    write };

bool operation_deadline_transitions_are_exclusive() {
    using deadline_type = ruvia::operation_deadline<deadline_kind>;
    deadline_type deadline;
    const auto now = deadline_type::clock_type::time_point{};
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
    if (deadline.kind() != nullptr || deadline.expired() || deadline.clear()) {
        return false;
    }

    ruvia::operation_deadline<> unlabelled;
    unlabelled.arm(std::optional<ruvia::timeout_duration>{});
    if (unlabelled.kind() != nullptr || unlabelled.expired()) {
        return false;
    }
    unlabelled.arm(ruvia::timeout_duration{});
    return unlabelled.kind() != nullptr &&
           unlabelled.expire(ruvia::operation_deadline<>::clock_type::now()).has_value() &&
           unlabelled.expired() && unlabelled.clear();
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
        expired.remaining() != ruvia::timeout_duration{}) {
        return false;
    }

    const timeout_type active(std::chrono::seconds(1));
    const auto deadline_value = active.deadline();
    const auto remaining = active.remaining();
    if (!deadline_value.has_value() || !remaining.has_value() || remaining->value().count() <= 0 ||
        remaining->value() > std::chrono::seconds(1)) {
        return false;
    }
    return active.constrained_by(std::chrono::seconds(2)).deadline() == deadline_value &&
           unlimited.constrained_by(std::chrono::seconds(2)).deadline().has_value();
}

}  // namespace

TEST(Ruvia, OperationDeadlineTransitionsAreExclusive) {
    EXPECT_TRUE(operation_deadline_transitions_are_exclusive());
}

TEST(Ruvia, OperationTimeoutUsesOneAbsoluteDeadline) {
    EXPECT_TRUE(operation_timeout_uses_one_absolute_deadline());
}

TEST(Ruvia, timeout_duration_checks_the_original_integer_domain) {
    using wide_milliseconds = std::chrono::duration<std::uint64_t, std::milli>;
    constexpr wide_milliseconds wide(std::numeric_limits<std::uint64_t>::max());
    constexpr ruvia::timeout_duration saturated(wide);
    static_assert(saturated.value() == std::chrono::milliseconds::max());
    static_assert(!std::is_convertible_v<ruvia::timeout_duration, std::chrono::milliseconds>);
    static_assert(!std::is_constructible_v<ruvia::timeout_duration, std::chrono::duration<double>>);

    ruvia::operation_options options{.timeout_ = wide};
    ASSERT_TRUE(options.timeout_.has_value());
    EXPECT_EQ(options.timeout_->value(), std::chrono::milliseconds::max());
    ruvia::validate_operation_options(options);

    options.timeout_ = std::chrono::nanoseconds(1);
    EXPECT_EQ(options.timeout_->value(), std::chrono::milliseconds(1));
    ruvia::validate_operation_options(options);

    using thirds = std::chrono::duration<std::int64_t, std::ratio<1, 3>>;
    EXPECT_EQ(ruvia::timeout_duration(thirds(7)).value(), std::chrono::milliseconds(2334));
    EXPECT_EQ(ruvia::timeout_duration(thirds(-7)).value(), std::chrono::milliseconds(-2333));
    using wide_hours = std::chrono::duration<std::int64_t, std::ratio<3600>>;
    EXPECT_EQ(ruvia::timeout_duration(wide_hours::min()).value(),
        std::chrono::milliseconds::min());

    options.timeout_ = std::chrono::nanoseconds(-1);
    EXPECT_THROW(ruvia::validate_operation_options(options), std::invalid_argument);
}

TEST(Ruvia, wide_positive_timeouts_remain_active_and_can_be_tightened) {
    using wide_milliseconds = std::chrono::duration<std::uint64_t, std::milli>;
    const wide_milliseconds wide(std::numeric_limits<std::uint64_t>::max());
    const ruvia::operation_timeout timeout(wide);
    EXPECT_FALSE(timeout.expired());
    ASSERT_TRUE(timeout.remaining().has_value());
    EXPECT_GT(timeout.remaining()->value(), std::chrono::milliseconds::zero());

    const ruvia::operation_timeout unlimited(std::nullopt);
    EXPECT_FALSE(unlimited.constrained_by(wide).expired());
    ruvia::operation_deadline<> deadline;
    deadline.arm(wide);
    EXPECT_FALSE(deadline.expire(std::chrono::steady_clock::now()).has_value());
    EXPECT_TRUE(deadline.expire(std::chrono::steady_clock::time_point::max()).has_value());

    const auto tightened = ruvia::override_operation_options(
        {.timeout_ = wide}, {.timeout_ = std::chrono::nanoseconds(1)});
    ASSERT_TRUE(tightened.timeout_.has_value());
    EXPECT_EQ(tightened.timeout_->value(), std::chrono::milliseconds(1));
    const auto retained = ruvia::override_operation_options(tightened, {.timeout_ = wide});
    EXPECT_EQ(retained.timeout_, tightened.timeout_);
}

TEST(Ruvia, timer_conversion_helpers_do_not_narrow_before_saturation) {
    using clock_type = std::chrono::steady_clock;
    using wide_ticks = std::chrono::duration<std::uint64_t, clock_type::duration::period>;
    const wide_ticks ticks(std::numeric_limits<std::uint64_t>::max());
    EXPECT_EQ(ruvia::worker_timer_saturating_deadline(clock_type::time_point{}, ticks),
        clock_type::time_point::max());

    using wide_milliseconds = std::chrono::duration<std::uint64_t, std::milli>;
    const wide_milliseconds milliseconds(std::numeric_limits<std::uint64_t>::max());
    EXPECT_EQ(ruvia::worker_timer_ceil_milliseconds(milliseconds),
        std::chrono::milliseconds::max());
    EXPECT_EQ(ruvia::worker_timer_ceil_milliseconds(std::chrono::nanoseconds(1)),
        std::chrono::milliseconds(1));
}
