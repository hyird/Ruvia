#include <gtest/gtest.h>

#include <chrono>
#include <concepts>
#include <cstdint>
#include <limits>
#include <optional>
#include <ratio>
#include <stdexcept>
#include <type_traits>

#include "ruvia/core/operation_options.h"
#include "ruvia/core/worker_timer.h"

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

bool deadline_lifecycle_transitions_are_exclusive() {
    using deadline_type = ruvia::deadline_lifecycle<deadline_kind>;
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

    ruvia::deadline_lifecycle<> unlabelled;
    if (unlabelled.kind() != nullptr || unlabelled.expired()) {
        return false;
    }
    unlabelled.arm(ruvia::deadline_lifecycle<>::clock_type::now());
    return unlabelled.kind() != nullptr &&
           unlabelled.expire(ruvia::deadline_lifecycle<>::clock_type::now()).has_value() &&
           unlabelled.expired() && unlabelled.clear();
}

bool operation_deadline_uses_one_absolute_deadline() {
    using deadline_type = ruvia::operation_deadline;
    const deadline_type unlimited;
    if (unlimited.deadline().has_value() || unlimited.remaining().has_value() || unlimited.expired() ||
        unlimited.timeout().has_value()) {
        return false;
    }

    const deadline_type expired(deadline_type::clock_type::time_point{});
    if (!expired.deadline().has_value() || !expired.expired() ||
        expired.remaining() != std::chrono::milliseconds::zero()) {
        return false;
    }

    const deadline_type active(std::optional<ruvia::operation_timeout>(std::chrono::seconds(1)));
    const auto deadline_value = active.deadline();
    const auto remaining = active.remaining();
    if (!deadline_value.has_value() || !remaining.has_value() || remaining->count() <= 0 ||
        *remaining > std::chrono::seconds(1)) {
        return false;
    }
    // Nested operations receive the same absolute deadline, not a fresh relative bound.
    const auto forwarded = active.timeout();
    if (!forwarded.has_value() || forwarded->deadline() != deadline_value || forwarded->duration().has_value()) {
        return false;
    }
    return active.constrained_by(std::chrono::seconds(2)).deadline() == deadline_value &&
           unlimited.constrained_by(std::chrono::seconds(2)).deadline().has_value() &&
           !unlimited.constrained_by(std::nullopt).deadline().has_value();
}

}  // namespace

TEST(Ruvia, DeadlineLifecycleTransitionsAreExclusive) {
    EXPECT_TRUE(deadline_lifecycle_transitions_are_exclusive());
}

TEST(Ruvia, OperationDeadlineUsesOneAbsoluteDeadline) {
    EXPECT_TRUE(operation_deadline_uses_one_absolute_deadline());
}

TEST(Ruvia, operation_timeout_is_valid_by_construction) {
    using clock_type = ruvia::operation_timeout::clock_type;
    static_assert(std::is_convertible_v<std::chrono::seconds, ruvia::operation_timeout>);
    static_assert(std::is_convertible_v<ruvia::timeout_duration, ruvia::operation_timeout>);
    static_assert(!std::is_convertible_v<clock_type::time_point, ruvia::operation_timeout>);
    static_assert(!std::is_constructible_v<ruvia::operation_timeout, std::chrono::duration<double>>);

    const ruvia::operation_timeout relative = std::chrono::nanoseconds(1);
    EXPECT_EQ(relative.duration(), std::chrono::milliseconds(1));
    EXPECT_FALSE(relative.deadline().has_value());
    EXPECT_EQ(ruvia::operation_timeout(ruvia::timeout_duration(std::chrono::seconds(2))).duration(),
        std::chrono::milliseconds(2000));
    EXPECT_THROW(ruvia::operation_timeout(std::chrono::milliseconds(0)), std::invalid_argument);
    EXPECT_THROW(ruvia::operation_timeout(std::chrono::nanoseconds(-1)), std::invalid_argument);
    EXPECT_THROW(ruvia::operation_timeout(ruvia::timeout_duration{}), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(ruvia::operation_options{.timeout_ = std::chrono::seconds(-1)}), std::invalid_argument);

    // An absolute deadline is accepted as given, including one that has passed.
    const ruvia::operation_timeout passed(clock_type::time_point{});
    EXPECT_EQ(passed.deadline(), clock_type::time_point{});
    EXPECT_FALSE(passed.duration().has_value());
    const auto start = clock_type::now();
    EXPECT_EQ(passed.deadline_from(start), clock_type::time_point{});
    EXPECT_EQ(relative.deadline_from(start), start + std::chrono::milliseconds(1));
    EXPECT_EQ(ruvia::operation_timeout(std::chrono::hours::max()).deadline_from(start), clock_type::time_point::max());

    EXPECT_FALSE(ruvia::operation_timeout::try_create(std::chrono::milliseconds(0)).has_value());
    EXPECT_FALSE(ruvia::operation_timeout::try_create(std::chrono::seconds(-3)).has_value());
    EXPECT_FALSE(ruvia::operation_timeout::try_create(ruvia::timeout_duration{}).has_value());
    EXPECT_EQ(ruvia::operation_timeout::try_create(std::chrono::microseconds(1500)),
        ruvia::operation_timeout(std::chrono::milliseconds(2)));
    EXPECT_EQ(ruvia::operation_timeout::try_create(ruvia::timeout_duration(std::chrono::seconds(1))),
        ruvia::operation_timeout(std::chrono::seconds(1)));
}

TEST(Ruvia, operation_timeout_overrides_only_tighten) {
    using clock_type = ruvia::operation_timeout::clock_type;
    // Two relative limits keep the shorter.
    auto merged = ruvia::override_operation_options({.timeout_ = std::chrono::seconds(5)}, {.timeout_ = std::chrono::seconds(1)});
    EXPECT_EQ(merged.timeout_, ruvia::operation_timeout(std::chrono::seconds(1)));
    merged = ruvia::override_operation_options({.timeout_ = std::chrono::seconds(1)}, {.timeout_ = std::chrono::seconds(5)});
    EXPECT_EQ(merged.timeout_, ruvia::operation_timeout(std::chrono::seconds(1)));
    // Two deadlines keep the earlier.
    const auto early = clock_type::now() + std::chrono::seconds(1);
    const auto late = early + std::chrono::seconds(10);
    merged = ruvia::override_operation_options(
        {.timeout_ = ruvia::operation_timeout(late)}, {.timeout_ = ruvia::operation_timeout(early)});
    EXPECT_EQ(merged.timeout_->deadline(), early);
    merged = ruvia::override_operation_options(
        {.timeout_ = ruvia::operation_timeout(early)}, {.timeout_ = ruvia::operation_timeout(late)});
    EXPECT_EQ(merged.timeout_->deadline(), early);
    // A mixed pair resolves the relative limit at merge time and keeps the earlier deadline.
    merged = ruvia::override_operation_options({.timeout_ = std::chrono::hours(1)}, {.timeout_ = ruvia::operation_timeout(early)});
    EXPECT_EQ(merged.timeout_->deadline(), early);
    const auto before = clock_type::now();
    merged = ruvia::override_operation_options({.timeout_ = ruvia::operation_timeout(late)}, {.timeout_ = std::chrono::milliseconds(5)});
    ASSERT_TRUE(merged.timeout_.has_value());
    ASSERT_TRUE(merged.timeout_->deadline().has_value());
    EXPECT_GE(*merged.timeout_->deadline(), before + std::chrono::milliseconds(5));
    EXPECT_LT(*merged.timeout_->deadline(), late);
    // An unspecified override inherits; a supplied token replaces.
    ruvia::stop_source source;
    merged = ruvia::override_operation_options({.timeout_ = std::chrono::seconds(1)}, {.stop_token_ = source.token()});
    EXPECT_EQ(merged.timeout_, ruvia::operation_timeout(std::chrono::seconds(1)));
    EXPECT_TRUE(merged.stop_token_.has_state());
    merged = ruvia::override_operation_options({}, {.timeout_ = std::chrono::seconds(1)});
    EXPECT_EQ(merged.timeout_, ruvia::operation_timeout(std::chrono::seconds(1)));
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
    EXPECT_EQ(options.timeout_->duration(), std::chrono::milliseconds::max());

    options.timeout_ = std::chrono::nanoseconds(1);
    EXPECT_EQ(options.timeout_->duration(), std::chrono::milliseconds(1));

    using thirds = std::chrono::duration<std::int64_t, std::ratio<1, 3>>;
    EXPECT_EQ(ruvia::timeout_duration(thirds(7)).value(), std::chrono::milliseconds(2334));
    EXPECT_EQ(ruvia::timeout_duration(thirds(-7)).value(), std::chrono::milliseconds(-2333));
    using wide_hours = std::chrono::duration<std::int64_t, std::ratio<3600>>;
    EXPECT_EQ(ruvia::timeout_duration(wide_hours::min()).value(),
        std::chrono::milliseconds::min());
}

TEST(Ruvia, wide_positive_timeouts_remain_active_and_can_be_tightened) {
    using wide_milliseconds = std::chrono::duration<std::uint64_t, std::milli>;
    const wide_milliseconds wide(std::numeric_limits<std::uint64_t>::max());
    const ruvia::operation_deadline timeout{std::optional<ruvia::operation_timeout>(wide)};
    EXPECT_FALSE(timeout.expired());
    ASSERT_TRUE(timeout.remaining().has_value());
    EXPECT_GT(*timeout.remaining(), std::chrono::milliseconds::zero());

    const ruvia::operation_deadline unlimited;
    EXPECT_FALSE(unlimited.constrained_by(wide).expired());
    ruvia::deadline_lifecycle<> deadline;
    deadline.arm(*timeout.deadline());
    EXPECT_FALSE(deadline.expire(std::chrono::steady_clock::now()).has_value());
    EXPECT_TRUE(deadline.expire(std::chrono::steady_clock::time_point::max()).has_value());

    const auto tightened = ruvia::override_operation_options(
        {.timeout_ = wide}, {.timeout_ = std::chrono::nanoseconds(1)});
    ASSERT_TRUE(tightened.timeout_.has_value());
    EXPECT_EQ(tightened.timeout_->duration(), std::chrono::milliseconds(1));
    const auto retained = ruvia::override_operation_options(tightened, {.timeout_ = wide});
    EXPECT_EQ(retained.timeout_, tightened.timeout_);
}

TEST(Ruvia, timer_conversions_do_not_narrow_before_saturation) {
    using clock_type = std::chrono::steady_clock;
    using wide_ticks = std::chrono::duration<std::uint64_t, clock_type::duration::period>;
    const wide_ticks ticks(std::numeric_limits<std::uint64_t>::max());
    EXPECT_EQ(ruvia::saturating_deadline(clock_type::time_point{}, ticks),
        clock_type::time_point::max());

    using wide_milliseconds = std::chrono::duration<std::uint64_t, std::milli>;
    const wide_milliseconds milliseconds(std::numeric_limits<std::uint64_t>::max());
    EXPECT_EQ(ruvia::ceil_milliseconds(milliseconds),
        std::chrono::milliseconds::max());
    EXPECT_EQ(ruvia::ceil_milliseconds(std::chrono::nanoseconds(1)),
        std::chrono::milliseconds(1));
}
