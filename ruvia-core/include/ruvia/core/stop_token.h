#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/core/worker_timer.h"

namespace ruvia {

class stop_token;
class stop_source;
class stop_registration;

namespace detail {

class stop_state;
class stop_callback_node;

enum class stop_registration_phase : unsigned char {
    idle,
    registering,
    registered,
    reset_pending,
    resetting,
};

// Shared ownership of one stop_state. Copies retain it; the last owner, on
// whichever thread it retires, returns the state to its memory resource.
class stop_state_ref final {
public:
    stop_state_ref() noexcept = default;
    stop_state_ref(const stop_state_ref& other) noexcept;
    stop_state_ref(stop_state_ref&& other) noexcept
        : state_(std::exchange(other.state_, nullptr)) {}
    stop_state_ref& operator=(stop_state_ref other) noexcept {
        std::swap(state_, other.state_);
        return *this;
    }
    ~stop_state_ref();

    [[nodiscard]] stop_state* operator->() const noexcept {
        return state_;
    }
    // Whether this refers to a stop state at all. Unlike stoppable(), this
    // stays true after the source died unstopped.
    [[nodiscard]] explicit operator bool() const noexcept {
        return state_ != nullptr;
    }
    [[nodiscard]] friend bool operator==(const stop_state_ref&, const stop_state_ref&) noexcept = default;

private:
    friend class stop_state;

    // Adopts a reference the caller already holds.
    explicit stop_state_ref(stop_state* adopted) noexcept
        : state_(adopted) {}

    stop_state* state_{nullptr};
};

// The state one stop_source shares with its tokens and registrations. It is
// allocated from the process-lifetime mimalloc resource, which supports
// releasing its last owner on any thread, even after the allocating thread exits.
//
// Callbacks form an intrusive list under a mutex. request_stop() runs each one
// with the mutex released; unlinking a callback that is running on another
// thread waits for it to return, while a callback unlinking itself does not.
class stop_state final {
public:
    stop_state(const stop_state&) = delete;
    stop_state& operator=(const stop_state&) = delete;

    [[nodiscard]] static stop_state_ref create();
    void retain() noexcept {
        references_.fetch_add(1, std::memory_order_relaxed);
    }
    void release() noexcept {
        if (references_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            destroy();
        }
    }

    [[nodiscard]] bool stop_requested() const noexcept {
        return (flags_.load(std::memory_order_acquire) & requested_flag) != 0;
    }
    // A stop is possible while the source lives or once it has been requested.
    [[nodiscard]] bool stop_possible() const noexcept {
        return flags_.load(std::memory_order_acquire) != 0;
    }
    void retire_source() noexcept {
        flags_.fetch_and(static_cast<std::uint8_t>(~source_flag), std::memory_order_acq_rel);
    }
    void request_stop() noexcept;

private:
    friend class ::ruvia::stop_registration;
    friend class stop_callback_node;

    static constexpr std::uint8_t source_flag = 1;
    static constexpr std::uint8_t requested_flag = 2;

    stop_state() noexcept = default;
    ~stop_state() = default;

    void destroy() noexcept;
    // Returns false, linking nothing, when stop was already requested or
    // registration was reset before linking.
    [[nodiscard]] bool link(stop_callback_node& node, std::atomic<stop_registration_phase>& phase) noexcept;
    void unlink(stop_callback_node& node) noexcept;

    std::atomic<std::uint8_t> flags_{source_flag};
    std::atomic<std::size_t> references_{1};
    std::mutex mutex_;
    stop_callback_node* head_{nullptr};
    // The callback request_stop() is running with the mutex released, and the
    // thread running it.
    stop_callback_node* running_{nullptr};
    std::thread::id requester_;
    // An unlink on another thread blocks on run_epoch_ until the running
    // callback returns. The wait lives here, not in the node: the requester
    // must not touch a node after its run, since the waiter may free it, while
    // the state outlives both through the node's and the requester's references.
    bool run_awaited_{false};
    std::atomic<std::uint32_t> run_epoch_{0};
};

inline stop_state_ref::stop_state_ref(const stop_state_ref& other) noexcept
    : state_(other.state_) {
    if (state_ != nullptr) {
        state_->retain();
    }
}

inline stop_state_ref::~stop_state_ref() {
    if (state_ != nullptr) {
        state_->release();
    }
}

// One registration's link into one stop state. Destruction unlinks a linked
// node; if the callback is running on another thread, it waits for that run to
// return before destroying the stored callback.
class stop_callback_node final {
public:
    stop_callback_node(const stop_state_ref& state_value, move_only_function<void()> callback_value) noexcept
        : state_(state_value),
          callback_(std::move(callback_value)) {}
    ~stop_callback_node() {
        if (linked_) {
            state_->unlink(*this);
        }
    }

    stop_callback_node(const stop_callback_node&) = delete;
    stop_callback_node& operator=(const stop_callback_node&) = delete;
    stop_callback_node(stop_callback_node&&) = delete;
    stop_callback_node& operator=(stop_callback_node&&) = delete;

    [[nodiscard]] move_only_function<void()> take() noexcept {
        if (invoked_.test_and_set(std::memory_order_acq_rel)) {
            return {};
        }
        return std::move(callback_);
    }

    static void invoke(move_only_function<void()>&& callback) noexcept {
        if (!callback) {
            return;
        }
        try {
            callback();
        } catch (...) {
            std::terminate();
        }
    }

    void invoke() noexcept {
        auto callback = take();
        invoke(std::move(callback));
    }

private:
    friend class ::ruvia::stop_registration;
    friend class stop_state;

    stop_state_ref state_;
    move_only_function<void()> callback_;
    stop_callback_node* previous_{nullptr};
    stop_callback_node* next_{nullptr};
    std::atomic_flag invoked_{};
    bool linked_{false};
};

}  // namespace detail

class stop_registration final {
public:
    stop_registration() noexcept = default;
    ~stop_registration() {
        reset();
    }

    stop_registration(const stop_registration&) = delete;
    stop_registration& operator=(const stop_registration&) = delete;
    stop_registration(stop_registration&&) = delete;
    stop_registration& operator=(stop_registration&&) = delete;

    void reset() noexcept {
        auto phase = phase_.load(std::memory_order_acquire);
        for (;;) {
            if (phase == registration_phase_type::idle || phase == registration_phase_type::reset_pending ||
                phase == registration_phase_type::resetting) {
                return;
            }
            if (phase == registration_phase_type::registering) {
                if (phase_.compare_exchange_weak(phase, registration_phase_type::reset_pending,
                        std::memory_order_acq_rel, std::memory_order_acquire)) {
                    return;
                }
                continue;
            }
            if (phase_.compare_exchange_weak(phase, registration_phase_type::resetting,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                clear_registration();
                phase_.store(registration_phase_type::idle, std::memory_order_release);
                return;
            }
        }
    }

    [[nodiscard]] bool registered() const noexcept {
        return phase_.load(std::memory_order_acquire) == registration_phase_type::registered;
    }

private:
    friend class stop_token;

    using registration_phase_type = detail::stop_registration_phase;

    stop_registration(const detail::stop_state_ref& state, move_only_function<void()> callback_value) {
        register_callback(state, std::move(callback_value));
    }

    void register_callback(const detail::stop_state_ref& state, move_only_function<void()> callback_value) {
        if (!callback_value || !state || !state->stop_possible()) {
            return;
        }
        if (state->stop_requested()) {
            detail::stop_callback_node::invoke(std::move(callback_value));
            return;
        }

        auto expected = registration_phase_type::idle;
        if (!phase_.compare_exchange_strong(expected, registration_phase_type::registering,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            std::terminate();
        }
        callback_.emplace(state, std::move(callback_value));
        if (state->link(*callback_, phase_)) {
            return;
        }
        auto callback = callback_->take();
        callback_.reset();
        const auto previous_phase =
            phase_.exchange(registration_phase_type::idle, std::memory_order_acq_rel);
        if (previous_phase != registration_phase_type::reset_pending) {
            detail::stop_callback_node::invoke(std::move(callback));
        }
    }

    // Unlink waits for an invocation on another thread before retiring its state.
    void clear_registration() noexcept {
        callback_.reset();
    }

    std::atomic<registration_phase_type> phase_{registration_phase_type::idle};
    std::optional<detail::stop_callback_node> callback_;
};

// A copyable reference to one cancellation state. Combining cancellation requires
// an explicit address-stable stop_group owner, not additional state in a token.
class stop_token final {
public:
    stop_token() noexcept = default;

    [[nodiscard]] bool stop_requested() const noexcept {
        return state_ && state_->stop_requested();
    }

    [[nodiscard]] bool stoppable() const noexcept {
        return state_ && state_->stop_possible();
    }

    // Distinguishes an unspecified token from a state whose source has retired.
    [[nodiscard]] bool has_state() const noexcept {
        return static_cast<bool>(state_);
    }

    [[nodiscard]] friend bool operator==(const stop_token&, const stop_token&) noexcept = default;

    // The registration embeds one intrusive node; registration adds no allocation.
    // Registration may race with request_stop(). If stop was already requested,
    // the callback runs synchronously before registration returns.
    // request_stop() may invoke the callback on the requesting thread; callbacks
    // that affect worker-owned state must post only an id/generation to that
    // worker and let the owner validate it there.
    // Callback retirement or moves may destroy this token; retain its entry state.
    [[nodiscard]] stop_registration register_callback(move_only_function<void()> callback_value) const {
        const auto state = state_;
        return stop_registration(state, std::move(callback_value));
    }

    // Reuses caller-owned registration storage, replacing any earlier
    // registration. This is for awaiters that can only form their cancellation
    // callback after an operation has published its id/generation in
    // await_suspend; it preserves the embedded, zero-allocation node
    // representation.
    void register_callback(stop_registration& registration, move_only_function<void()> callback_value) const {
        const auto state = state_;
        registration.reset();
        registration.register_callback(state, std::move(callback_value));
    }

private:
    friend class stop_source;
    friend class stop_group;

    explicit stop_token(detail::stop_state_ref state_value) noexcept
        : state_(std::move(state_value)) {}

    detail::stop_state_ref state_;
};

// Owns one stop state and requests its stop. The state comes from
// the process-lifetime mimalloc resource and lives until this source and every
// token and registration observing it retire, on whichever thread that is.
class stop_source final {
public:
    stop_source();
    ~stop_source() {
        state_->retire_source();
    }
    stop_source(const stop_source&) = delete;
    stop_source& operator=(const stop_source&) = delete;
    stop_source(stop_source&&) = delete;
    stop_source& operator=(stop_source&&) = delete;

    // Callbacks run on this thread before it returns. One of them may destroy
    // this source, so the request holds its own reference to the state.
    void request_stop() noexcept {
        const auto state_value = state_;
        state_value->request_stop();
    }

    [[nodiscard]] bool stop_requested() const noexcept {
        return state_->stop_requested();
    }

    [[nodiscard]] stop_token token() const noexcept {
        return stop_token(state_);
    }

private:
    detail::stop_state_ref state_;
};

// Address-stable owner of parent registrations and one downstream cancellation
// state. Keep it alive until all operations using token() have completed/joined.
// Zero/one distinct stoppable parent is passed through without a new state.
// Multiple parents share one output state, so duplicate/nested sources notify
// each downstream registration once. Only parent counts above two allocate
// registration storage; ordinary stop_registration always contains one node.
// Destruction first unregisters parents (waiting for callbacks on other threads).
class stop_group final {
public:
    explicit stop_group(std::span<const stop_token> parents);
    stop_group(std::initializer_list<stop_token> parents)
        : stop_group(std::span<const stop_token>(parents.begin(), parents.size())) {}
    stop_group(const stop_token& first, const stop_token& second)
        : stop_group(std::span<const stop_token>(std::array<stop_token, 2>{first, second})) {}
    ~stop_group() = default;

    stop_group(const stop_group&) = delete;
    stop_group& operator=(const stop_group&) = delete;
    stop_group(stop_group&&) = delete;
    stop_group& operator=(stop_group&&) = delete;

    [[nodiscard]] stop_token token() const noexcept {
        return output_;
    }

private:
    std::optional<stop_source> source_;
    stop_token output_;
    std::array<stop_registration, 2> parents_;
    std::vector<stop_registration> extra_parents_;
};

}  // namespace ruvia

namespace ruvia {

// The bound of one operation: either a positive relative limit that starts
// when the operation starts, or an absolute steady-clock deadline, which may
// already have passed. A value is valid by construction: the relative
// constructors throw std::invalid_argument for a non-positive limit, and
// try_create() is the tolerant form for values that may legitimately be
// non-positive (configuration, remaining budgets).
class operation_timeout final {
public:
    using clock_type = std::chrono::steady_clock;

    // Implicit so `{.timeout_ = 5s}` stays writable. Rounds up to whole
    // milliseconds and saturates, so a positive input is at least 1 ms.
    template <std::integral rep_type, typename period_type>
    operation_timeout(std::chrono::duration<rep_type, period_type> value)
        : value_(require_positive(saturating_duration_cast<std::chrono::milliseconds>(value), value.count() > 0)) {}

    operation_timeout(timeout_duration value)
        : value_(require_positive(value.value(), value.value().count() > 0)) {}

    explicit operation_timeout(clock_type::time_point deadline) noexcept
        : value_(deadline) {}

    template <std::integral rep_type, typename period_type>
    [[nodiscard]] static std::optional<operation_timeout> try_create(
        std::chrono::duration<rep_type, period_type> value) noexcept {
        if (value.count() <= 0) {
            return std::nullopt;
        }
        return operation_timeout(relative_tag{}, saturating_duration_cast<std::chrono::milliseconds>(value));
    }

    [[nodiscard]] static std::optional<operation_timeout> try_create(timeout_duration value) noexcept {
        if (value.value().count() <= 0) {
            return std::nullopt;
        }
        return operation_timeout(relative_tag{}, value.value());
    }

    // The relative limit; empty for an absolute deadline.
    [[nodiscard]] std::optional<std::chrono::milliseconds> duration() const noexcept {
        if (const auto* relative = std::get_if<std::chrono::milliseconds>(&value_)) {
            return *relative;
        }
        return std::nullopt;
    }

    // The absolute deadline; empty for a relative limit.
    [[nodiscard]] std::optional<clock_type::time_point> deadline() const noexcept {
        if (const auto* absolute = std::get_if<clock_type::time_point>(&value_)) {
            return *absolute;
        }
        return std::nullopt;
    }

    // Resolves the bound for an operation that starts at `start`.
    [[nodiscard]] clock_type::time_point deadline_from(clock_type::time_point start) const noexcept {
        if (const auto* relative = std::get_if<std::chrono::milliseconds>(&value_)) {
            return saturating_deadline(start, *relative);
        }
        return std::get<clock_type::time_point>(value_);
    }

    friend bool operator==(const operation_timeout&, const operation_timeout&) noexcept = default;

private:
    struct relative_tag final {};

    operation_timeout(relative_tag, std::chrono::milliseconds value) noexcept
        : value_(value) {}

    [[nodiscard]] static std::chrono::milliseconds require_positive(std::chrono::milliseconds value, bool positive) {
        if (!positive) {
            throw std::invalid_argument("operation timeout must be greater than zero");
        }
        return value;
    }

    std::variant<std::chrono::milliseconds, clock_type::time_point> value_;
};

}  // namespace ruvia

namespace ruvia {

// The single per-operation input of every cold wait and outbound operation.
// timeout_ bounds the operation end to end: a relative limit starts when the
// operation starts, an absolute deadline is used as given.
struct operation_options final {
    std::optional<operation_timeout> timeout_{};
    stop_token stop_token_{};
};

// Unspecified fields inherit. A supplied token replaces the user cancellation
// state; callers needing accumulation own a stop_group until their work joins.
// A supplied timeout may tighten but never relax the inherited bound: two
// relative limits keep the shorter, two deadlines keep the earlier, and a
// mixed pair resolves the relative limit against the merge time and keeps the
// earlier deadline.
[[nodiscard]] inline operation_options override_operation_options(
    const operation_options& base, operation_options overrides) {
    operation_options merged = base;
    if (overrides.timeout_.has_value()) {
        if (!merged.timeout_.has_value()) {
            merged.timeout_ = overrides.timeout_;
        } else {
            const auto& current = *merged.timeout_;
            const auto& candidate = *overrides.timeout_;
            const auto current_duration = current.duration();
            const auto candidate_duration = candidate.duration();
            if (current_duration.has_value() && candidate_duration.has_value()) {
                if (*candidate_duration < *current_duration) {
                    merged.timeout_ = candidate;
                }
            } else {
                const auto now = operation_timeout::clock_type::now();
                const auto current_deadline = current.deadline_from(now);
                const auto candidate_deadline = candidate.deadline_from(now);
                merged.timeout_ = operation_timeout(std::min(current_deadline, candidate_deadline));
            }
        }
    }
    if (overrides.stop_token_.has_state()) {
        merged.stop_token_ = std::move(overrides.stop_token_);
    }
    return merged;
}

}  // namespace ruvia

namespace ruvia {

// The resolved deadline of an operation that has started, shared across its
// asynchronous phases. Phases that hand the remaining budget to a nested
// operation pass timeout(), the same absolute deadline, rather than a fresh
// relative limit.
class operation_deadline final {
public:
    using clock_type = std::chrono::steady_clock;

    // Unbounded.
    operation_deadline() noexcept = default;

    // Resolves `timeout` against the current time; empty stays unbounded.
    explicit operation_deadline(const std::optional<operation_timeout>& timeout) noexcept {
        if (timeout.has_value()) {
            deadline_ = timeout->deadline_from(clock_type::now());
        }
    }

    explicit operation_deadline(clock_type::time_point deadline) noexcept
        : deadline_(deadline) {}

    [[nodiscard]] std::optional<clock_type::time_point> deadline() const noexcept {
        return deadline_;
    }

    // Zero once expired; positive remainders round up to whole milliseconds.
    [[nodiscard]] std::optional<std::chrono::milliseconds> remaining() const noexcept {
        if (!deadline_.has_value()) {
            return std::nullopt;
        }
        return ceil_milliseconds(*deadline_ - clock_type::now());
    }

    [[nodiscard]] bool expired() const noexcept {
        return deadline_.has_value() && clock_type::now() >= *deadline_;
    }

    // The earlier of this deadline and `timeout` resolved now.
    [[nodiscard]] operation_deadline constrained_by(const std::optional<operation_timeout>& timeout) const noexcept {
        operation_deadline constrained(timeout);
        if (deadline_.has_value() && (!constrained.deadline_.has_value() || *deadline_ < *constrained.deadline_)) {
            constrained.deadline_ = deadline_;
        }
        return constrained;
    }

    // The same deadline as an absolute operation_timeout for a nested
    // operation; empty when unbounded.
    [[nodiscard]] std::optional<operation_timeout> timeout() const noexcept {
        if (!deadline_.has_value()) {
            return std::nullopt;
        }
        return operation_timeout(*deadline_);
    }

private:
    std::optional<clock_type::time_point> deadline_;
};

// One allocation-free deadline lifecycle. A deadline is either absent, armed
// with its cancellation kind and exact time point, or expired with that same
// kind retained until the awaiting operation consumes the outcome.
template <typename kind_type = std::monostate>
    requires(std::is_enum_v<kind_type> || std::is_same_v<kind_type, std::monostate>)
class deadline_lifecycle final {
public:
    using clock_type = std::chrono::steady_clock;

    void arm(clock_type::time_point deadline_value, kind_type kind) noexcept {
        state_.template emplace<active>(deadline_value, std::move(kind));
    }

    void arm(clock_type::time_point deadline_value) noexcept
        requires std::is_same_v<kind_type, std::monostate>
    {
        arm(deadline_value, std::monostate{});
    }

    void reset() noexcept {
        state_.template emplace<inactive>();
    }

    [[nodiscard]] bool expired() const noexcept {
        return std::holds_alternative<expired_state>(state_);
    }

    [[nodiscard]] const kind_type* kind() const& noexcept {
        if (const auto* armed = std::get_if<active>(&state_)) {
            return &armed->kind_;
        }
        if (const auto* expired_value = std::get_if<expired_state>(&state_)) {
            return &expired_value->kind_;
        }
        return nullptr;
    }
    const kind_type* kind() const&& = delete;

    [[nodiscard]] std::optional<kind_type> expire(clock_type::time_point now) noexcept {
        const auto* armed = std::get_if<active>(&state_);
        if (armed == nullptr || armed->deadline_ > now) {
            return std::nullopt;
        }
        auto kind = armed->kind_;
        state_.template emplace<expired_state>(kind);
        return kind;
    }

    [[nodiscard]] bool clear() noexcept {
        const bool was_expired = expired();
        reset();
        return was_expired;
    }

private:
    struct inactive final {};

    struct active final {
        active(clock_type::time_point armed_deadline, kind_type armed_kind) noexcept
            : deadline_(armed_deadline),
              kind_(std::move(armed_kind)) {}

        clock_type::time_point deadline_;
        kind_type kind_;
    };

    struct expired_state final {
        explicit expired_state(kind_type expired_kind) noexcept
            : kind_(std::move(expired_kind)) {}

        kind_type kind_;
    };

    using state_type = std::variant<inactive, active, expired_state>;

    state_type state_;
};

}  // namespace ruvia
