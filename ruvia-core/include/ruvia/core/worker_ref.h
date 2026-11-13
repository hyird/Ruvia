#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cassert>
#include <chrono>
#include <cmath>
#include <compare>
#include <concepts>
#include <condition_variable>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <initializer_list>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <ratio>
#include <span>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <asio/io_context.hpp>

namespace ruvia {

namespace detail {

template <typename result_type, typename fn_type, typename... args_type>
concept move_only_function_target =
    (std::is_void_v<result_type> &&
        std::same_as<std::invoke_result_t<std::decay_t<fn_type>&, args_type...>, void>) ||
    (!std::is_void_v<result_type> &&
        std::is_invocable_r_v<result_type, std::decay_t<fn_type>&, args_type...> &&
        (!std::is_reference_v<result_type> ||
            (std::is_reference_v<std::invoke_result_t<std::decay_t<fn_type>&, args_type...>> &&
                std::is_convertible_v<
                    std::add_pointer_t<std::remove_reference_t<std::invoke_result_t<std::decay_t<fn_type>&, args_type...>>>,
                    std::add_pointer_t<std::remove_reference_t<result_type>>>)));

}  // namespace detail

template <typename signature_type>
class move_only_function;

namespace detail {

inline constexpr std::size_t move_only_function_inline_size = 3 * sizeof(void*);
inline constexpr std::size_t move_only_function_inline_alignment = alignof(std::max_align_t);

template <typename stored_type>
inline constexpr bool move_only_function_fits_inline =
    sizeof(stored_type) <= move_only_function_inline_size &&
    alignof(stored_type) <= move_only_function_inline_alignment &&
    std::is_nothrow_move_constructible_v<stored_type>;

template <typename signature_type, typename fn_type>
inline constexpr bool move_only_function_borrow_safe_input =
    !std::same_as<std::remove_cvref_t<fn_type>, move_only_function<signature_type>> &&
    move_only_function_fits_inline<std::decay_t<fn_type>> &&
    std::is_trivially_constructible_v<std::decay_t<fn_type>, fn_type&&> &&
    std::is_trivially_move_constructible_v<std::decay_t<fn_type>> &&
    std::is_trivially_destructible_v<std::decay_t<fn_type>>;

}  // namespace detail

template <typename result_type, bool is_noexcept, typename... args_type>
class move_only_function<result_type(args_type...) noexcept(is_noexcept)> final {
public:
    move_only_function() noexcept = default;
    move_only_function(std::nullptr_t) noexcept {}

    // 引用签名只接受直接返回兼容引用的目标（允许 cv 与派生到基类转换），
    // 不从返回值或转换临时量生成引用。自定义转换须由显式返回引用的 lambda
    // 完成；目标负责被引用对象的寿命。值签名仍允许正常隐式结果转换。
    template <typename fn_type>
        requires(!std::same_as<std::remove_cvref_t<fn_type>, move_only_function>) &&
                detail::move_only_function_target<result_type, fn_type, args_type...> &&
                (!is_noexcept || std::is_nothrow_invocable_r_v<result_type, std::decay_t<fn_type>&, args_type...>)
    move_only_function(fn_type&& fn) {
        using stored_type = std::decay_t<fn_type>;
        if constexpr (std::is_pointer_v<std::remove_reference_t<fn_type>> || std::is_member_pointer_v<std::remove_reference_t<fn_type>>) {
            if (fn == nullptr) {
                return;
            }
        }
        if constexpr (detail::move_only_function_fits_inline<stored_type>) {
            ::new (storage_) stored_type(std::forward<fn_type>(fn));
            object_ = storage_;
            operations_ = &inline_operations<stored_type>;
        } else {
            object_ = new stored_type(std::forward<fn_type>(fn));
            operations_ = &heap_operations<stored_type>;
        }
    }

    ~move_only_function() {
        reset();
    }

    move_only_function(const move_only_function&) = delete;
    move_only_function& operator=(const move_only_function&) = delete;

    move_only_function(move_only_function&& other) noexcept {
        move_from(other);
    }

    move_only_function& operator=(move_only_function&& other) noexcept {
        if (this != &other) {
            if (operations_ == nullptr) {
                move_from(other);
            } else {
                // Extract a source the old target may own before retiring it.
                move_only_function incoming(std::move(other));
                // Retire inline targets outside this wrapper's storage, and
                // commit before destruction so reentrant assignments win.
                move_only_function retired(std::move(*this));
                move_from(incoming);
                // Evaluate the return before retirement can destroy this wrapper.
                return *this;
            }
        }
        return *this;
    }

    move_only_function& operator=(std::nullptr_t) noexcept {
        return *this = move_only_function{};
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return operations_ != nullptr;
    }

    [[nodiscard]] friend constexpr bool operator==(const move_only_function& fn, std::nullptr_t) noexcept {
        return !fn;
    }

    // The value-parameter signature is part of the erased callable contract;
    // forwarding it preserves value and reference argument behavior.
    // Constness applies to the wrapper, not its target. Concurrent invocation
    // requires the target to synchronize its own mutable state.
    // NOLINTNEXTLINE(performance-unnecessary-value-param)
    result_type operator()(args_type... args) const noexcept(is_noexcept) {
        if (operations_ == nullptr) {
            std::terminate();
        }
        return operations_->invoke_(object_, std::forward<args_type>(args)...);
    }

private:
    // The members are initialized explicitly rather than left to default
    // construction: the operation tables below are const static aggregates, and
    // MSVC reports C4268 when such an object is zero-filled by a compiler
    // generated default constructor. Aggregate initialization is unaffected.
    struct operations final {
        result_type (*invoke_)(void*, args_type&&...) noexcept(is_noexcept) = nullptr;
        void (*destroy_)(void*) noexcept = nullptr;
        void (*move_)(void*, void*) noexcept = nullptr;
    };

    template <typename stored_type>
    static result_type invoke(void* object, args_type&&... args) noexcept(is_noexcept) {
        return std::invoke(*static_cast<stored_type*>(object), std::forward<args_type>(args)...);
    }

    template <typename stored_type>
    static void destroy_inline(void* object) noexcept {
        static_cast<stored_type*>(object)->~stored_type();
    }

    template <typename stored_type>
    static void move_inline(void* source_value, void* destination) noexcept {
        auto* stored = static_cast<stored_type*>(source_value);
        ::new (destination) stored_type(std::move(*stored));
        stored->~stored_type();
    }

    template <typename stored_type>
    static void destroy_heap(void* object) noexcept {
        delete static_cast<stored_type*>(object);
    }

    template <typename stored_type>
    static inline constexpr operations inline_operations{
        &invoke<stored_type>, &destroy_inline<stored_type>, &move_inline<stored_type>};

    // move_from() relocates only inline storage; heap targets keep their pointer.
    template <typename stored_type>
    static inline constexpr operations heap_operations{&invoke<stored_type>, &destroy_heap<stored_type>, nullptr};

    [[nodiscard]] bool is_inline() const noexcept {
        return object_ == static_cast<const void*>(storage_);
    }

    void reset() noexcept {
        // User destruction may reset or destroy this wrapper again.
        const auto* const operations_value = std::exchange(operations_, nullptr);
        void* const object_value = std::exchange(object_, nullptr);
        if (operations_value != nullptr) {
            operations_value->destroy_(object_value);
        }
    }

    void move_from(move_only_function& other) noexcept {
        // Moving or retiring the target may destroy its source wrapper.
        const bool inline_storage = other.is_inline();
        const auto* const operations_value = std::exchange(other.operations_, nullptr);
        void* const object_value = std::exchange(other.object_, nullptr);
        if (operations_value == nullptr) {
            return;
        }
        if (inline_storage) {
            operations_value->move_(object_value, storage_);
        }
        object_ = inline_storage ? storage_ : object_value;
        operations_ = operations_value;
    }

    alignas(detail::move_only_function_inline_alignment)
        std::byte storage_[detail::move_only_function_inline_size];
    void* object_{nullptr};
    const operations* operations_{nullptr};
};

}  // namespace ruvia

namespace ruvia {

// Converts directly from the original count domain to the requested tick type.
// Integral counts up to uintmax_t width stay exact, rounding positive fractions
// upward and saturating at both bounds. Other conversions use long double;
// NaN maps to zero, infinities saturate.
template <typename target_type = std::chrono::steady_clock::duration, typename rep_type, typename period_type>
[[nodiscard]] constexpr target_type saturating_duration_cast(
    std::chrono::duration<rep_type, period_type> value) noexcept {
    static_assert(std::is_integral_v<typename target_type::rep> && std::is_signed_v<typename target_type::rep>);
    using factor = std::ratio_divide<period_type, typename target_type::period>;
    if constexpr (std::is_integral_v<rep_type> &&
                  std::numeric_limits<rep_type>::digits <= std::numeric_limits<std::uintmax_t>::digits &&
                  (factor::num == 1 || factor::den == 1)) {
        using count_type = std::conditional_t<std::is_signed_v<rep_type>, std::intmax_t, std::uintmax_t>;
        const auto raw = static_cast<count_type>(value.count());
        auto count = raw / factor::den;
        if constexpr (factor::den > 1) {
            if (raw > 0 && raw % factor::den != 0) {
                if (std::cmp_greater_equal(count, target_type::max().count() / factor::num)) {
                    return target_type::max();
                }
                ++count;
            }
        }
        if (std::cmp_greater(count, target_type::max().count() / factor::num)) {
            return target_type::max();
        }
        if (std::cmp_less(count, target_type::min().count() / factor::num)) {
            return target_type::min();
        }
        return target_type(static_cast<typename target_type::rep>(count) * factor::num);
    } else if constexpr (std::is_integral_v<rep_type> &&
                         std::numeric_limits<rep_type>::digits <= std::numeric_limits<std::uintmax_t>::digits) {
        using count_type = std::conditional_t<std::is_signed_v<rep_type>, std::intmax_t, std::uintmax_t>;
        using target_rep = typename target_type::rep;
        const bool negative = std::cmp_less(static_cast<count_type>(value.count()), 0);
        const auto raw = static_cast<std::uintmax_t>(value.count());
        const auto magnitude = negative ? std::uintmax_t{0} - raw : raw;
        const auto limit = negative
                               ? std::uintmax_t{0} - static_cast<std::uintmax_t>(target_type::min().count())
                               : static_cast<std::uintmax_t>(target_type::max().count());
        constexpr auto numerator = static_cast<std::uintmax_t>(factor::num);
        constexpr auto denominator = static_cast<std::uintmax_t>(factor::den);
        const auto whole = magnitude / denominator;
        if (whole > limit / numerator) {
            return negative ? target_type::min() : target_type::max();
        }
        const auto integral = whole * numerator;
        const auto remainder = magnitude % denominator;
        std::uintmax_t fraction = 0;
        std::uintmax_t residue = 0;
        if (remainder <= (std::numeric_limits<std::uintmax_t>::max)() / numerator) {
            const auto product = remainder * numerator;
            fraction = product / denominator;
            residue = product % denominator;
        } else {
            // Divide the remainder product bitwise instead of overflowing it.
            // Each residue addition is below 2 * denominator <= UINTMAX_MAX.
            for (auto bit = std::bit_floor(numerator); bit != 0; bit >>= 1) {
                fraction *= 2;
                residue *= 2;
                if (residue >= denominator) {
                    residue -= denominator;
                    ++fraction;
                }
                if ((numerator & bit) != 0) {
                    residue += remainder;
                    if (residue >= denominator) {
                        residue -= denominator;
                        ++fraction;
                    }
                }
            }
        }
        if (!negative && residue != 0) {
            ++fraction;
        }
        if (fraction >= limit - integral) {
            return negative ? target_type::min() : target_type::max();
        }
        const auto ticks = static_cast<target_rep>(integral + fraction);
        return target_type(negative ? -ticks : ticks);
    } else {
        // Discard this alternative for integral conversions so MSVC /WX does
        // not reject an emitted unreachable fallback.
        using wide_type = std::chrono::duration<long double, typename target_type::period>;
        const auto count = std::chrono::duration_cast<wide_type>(value).count();
        if (std::isnan(count)) {
            return target_type::zero();
        }
        // Unit conversion may underflow before ceil sees the positive input.
        if (count == 0.0L && value.count() > 0) {
            return target_type(1);
        }
        const auto rounded = count > 0.0L ? std::ceil(count) : std::trunc(count);
        const auto maximum = static_cast<long double>(target_type::max().count());
        if (rounded >= maximum) {
            return target_type::max();
        }
        const auto minimum = static_cast<long double>(target_type::min().count());
        if (rounded <= minimum) {
            return target_type::min();
        }
        return target_type(static_cast<typename target_type::rep>(rounded));
    }
}

// Public relative-time inputs retain their original representation until this
// boundary. Non-positive values remain available to each consumer's policy.
class timeout_duration final {
public:
    constexpr timeout_duration() noexcept = default;

    template <std::integral rep_type, typename period_type>
    constexpr timeout_duration(std::chrono::duration<rep_type, period_type> value) noexcept
        : value_(saturating_duration_cast<std::chrono::milliseconds>(value)) {}

    template <std::integral count_type>
    explicit constexpr timeout_duration(count_type milliseconds) noexcept
        : timeout_duration(std::chrono::duration<count_type, std::milli>(milliseconds)) {}

    [[nodiscard]] constexpr std::chrono::milliseconds value() const noexcept {
        return value_;
    }

    // Mixed comparisons must unwrap explicitly: rounding elapsed time here
    // could otherwise make a positive fractional millisecond expire early.
    template <typename left_type, typename right_type>
        requires std::same_as<std::remove_cvref_t<left_type>, timeout_duration> &&
                 std::same_as<std::remove_cvref_t<right_type>, timeout_duration>
    friend constexpr auto operator<=>(left_type&& left, right_type&& right) noexcept {
        return left.value_ <=> right.value_;
    }

    template <typename left_type, typename right_type>
        requires std::same_as<std::remove_cvref_t<left_type>, timeout_duration> &&
                 std::same_as<std::remove_cvref_t<right_type>, timeout_duration>
    friend constexpr bool operator==(left_type&& left, right_type&& right) noexcept {
        return left.value_ == right.value_;
    }

private:
    std::chrono::milliseconds value_{};
};

template <typename rep_type, typename period_type>
[[nodiscard]] inline std::chrono::steady_clock::time_point saturating_deadline(
    std::chrono::steady_clock::time_point now, std::chrono::duration<rep_type, period_type> value) noexcept {
    const auto delay = saturating_duration_cast(value);
    if (delay <= std::chrono::steady_clock::duration::zero()) {
        return now;
    }
    constexpr auto maximum = std::chrono::steady_clock::time_point::max();
    if (now > maximum - delay) {
        return maximum;
    }
    return now + delay;
}

// Non-positive delays map to zero; positive delays round up to whole
// milliseconds and saturate at the representable maximum.
template <typename rep_type, typename period_type>
[[nodiscard]] constexpr std::chrono::milliseconds ceil_milliseconds(
    std::chrono::duration<rep_type, period_type> value) noexcept {
    if (value <= std::chrono::duration<rep_type, period_type>::zero()) {
        return std::chrono::milliseconds::zero();
    }
    return saturating_duration_cast<std::chrono::milliseconds>(value);
}

template <typename rep_type, typename period_type>
[[nodiscard]] inline std::chrono::steady_clock::time_point deadline_after(
    std::chrono::duration<rep_type, period_type> delay) noexcept {
    return saturating_deadline(std::chrono::steady_clock::now(), delay);
}

}  // namespace ruvia

namespace ruvia::detail {
class worker_dispatcher;
struct timer_slot;
}  // namespace ruvia::detail

namespace ruvia {

class worker_timer_cancellation final {
public:
    worker_timer_cancellation() noexcept = default;

    void cancel() const noexcept;
    [[nodiscard]] bool valid() const noexcept {
        return dispatcher_ != nullptr && generation_ != 0;
    }

private:
    // Borrows the dispatcher kept alive by the awaiter's stable worker_ref.
    // Stop callback teardown synchronizes the borrow; cancellation dispatch owns
    // any continuation it posts.
    worker_timer_cancellation(
        detail::worker_dispatcher& dispatcher, std::size_t slot, std::uint64_t generation) noexcept
        : dispatcher_(&dispatcher),
          slot_(slot),
          generation_(generation) {}

    detail::worker_dispatcher* dispatcher_{nullptr};
    std::size_t slot_{0};
    std::uint64_t generation_{0};

    friend class worker_timer_registration;
};

enum class worker_timer_outcome : std::uint8_t {
    expired,
    // cancel_and_notify(), a worker_timer_cancellation, or worker shutdown
    // (stop_timers) ended the pending timer.
    cancelled,
};

// Result of worker_ref::schedule_timer(). Shutdown is a normal state, not a
// failure: once the worker's timer queue has stopped, scheduling returns
// worker_stopping, stores nothing, never invokes the completion, and leaves the
// registration unbound. That is the same terminal point a pending timer reaches
// through stop_timers(), which delivers worker_timer_outcome::cancelled; timed
// primitives map both to their own shutdown result.
// This also applies when moving the completion itself stops the timer queue.
enum class worker_timer_schedule_status : std::uint8_t {
    scheduled,
    worker_stopping,
};

// Borrowed timer token. A registration is bound to the worker of its most
// recent schedule_timer() until that timer expires, is cancelled, or is
// released by cancel()/cancel_and_notify(). Rescheduling while registration
// is in progress or the timer is still pending throws std::logic_error. Once
// it has expired or been cancelled, it may be scheduled on another worker;
// that worker observes the release with acquire/release ordering, so a caller
// that learned of the expiry through its completion may reuse it.
class worker_timer_registration final {
public:
    worker_timer_registration() noexcept = default;
    ~worker_timer_registration();

    worker_timer_registration(const worker_timer_registration&) = delete;
    worker_timer_registration& operator=(const worker_timer_registration&) = delete;
    worker_timer_registration(worker_timer_registration&&) = delete;
    worker_timer_registration& operator=(worker_timer_registration&&) = delete;

    // Removes the registration without delivering its completion: the owner
    // that cancels has stopped observing the outcome (a finished operation,
    // a re-arm, setup rollback). Destruction does the same.
    void cancel() noexcept;
    // Removes the registration and delivers worker_timer_outcome::cancelled,
    // for a completion that must run to resume or release its waiter. On the
    // worker's own thread the completion is posted rather than re-entered.
    void cancel_and_notify() noexcept;
    [[nodiscard]] worker_timer_cancellation cancellation() const&;
    worker_timer_cancellation cancellation() const&& = delete;
    // Expiry consumes the queue entry without writing through this borrowed
    // registration; its stable owner releases or reuses the token afterwards.
    [[nodiscard]] bool registered() const noexcept;

private:
    void unregister(bool notify) noexcept;
    void bind(detail::worker_dispatcher& dispatcher, std::size_t slot, std::uint64_t generation,
        const detail::timer_slot& slot_state) noexcept;
    void release() noexcept;
    [[nodiscard]] bool pending() const noexcept;

    // The worker_ref supplied to schedule_timer() must outlive this registration.
    // Destruction only removes it; it never queues a callback referencing the
    // destroyed owner.
    detail::worker_dispatcher* dispatcher_{nullptr};
    // Owned by that dispatcher; its activity is readable from any thread.
    const detail::timer_slot* slot_state_{nullptr};
    std::size_t slot_{0};
    std::uint64_t generation_{0};
    bool scheduling_{false};

    friend class detail::worker_dispatcher;
};

}  // namespace ruvia

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

namespace ruvia {

// 64-bit FNV-1a parameters (non-cryptographic; dispatch and fingerprint use only).
inline constexpr std::uint64_t fnv1a_offset_basis = 14695981039346656037ULL;
inline constexpr std::uint64_t fnv1a_prime = 1099511628211ULL;

[[nodiscard]] constexpr std::uint64_t fnv1a_accumulate(std::uint64_t hash, std::string_view bytes) noexcept {
    for (const char byte : bytes) {
        hash ^= static_cast<unsigned char>(byte);
        hash *= fnv1a_prime;
    }
    return hash;
}

// Mixes an integer as its little-endian bytes so results do not depend on host order.
[[nodiscard]] constexpr std::uint64_t fnv1a_accumulate_integer(std::uint64_t hash, std::uint64_t value) noexcept {
    for (std::size_t i = 0; i < sizeof(value); ++i) {
        hash ^= (value >> (i * 8U)) & 0xffU;
        hash *= fnv1a_prime;
    }
    return hash;
}

}  // namespace ruvia

namespace ruvia {
template <typename signature_type>
class move_only_function;
}  // namespace ruvia

namespace ruvia::detail {
template <typename t_type>
class worker_wait_value final {
public:
    template <typename u_type>
        requires std::constructible_from<t_type, u_type&&>
    explicit worker_wait_value(u_type&& value) noexcept(std::is_nothrow_constructible_v<t_type, u_type&&>)
        : value_(std::forward<u_type>(value)) {}

    t_type value_;
};

template <typename t_type>
inline constexpr bool is_move_only_function = false;
template <typename signature_type>
inline constexpr bool is_move_only_function<::ruvia::move_only_function<signature_type>> = true;

struct worker_delivery_access;
}  // namespace ruvia::detail

namespace ruvia {

enum class worker_wait_status : std::uint8_t {
    value,
    closed,
    // The bound worker is shutting down: its stopping notification arrived, or
    // a timed wait found (or was pending when) the worker's timer queue stopped.
    worker_stopping,
    timed_out,
    cancelled,
};

// Worker-bound waits have five mutually exclusive outcomes. A status is always
// available for exhaustive handling; only value owns a payload.
// The outcome category is fixed at construction. Construction and extraction
// follow the payload's copy/move semantics; a failed payload move never leaves
// an existing outcome without a category. Rebuild an owning optional to replace
// an outcome instead of assigning it. with_value() and with_status() let a
// composed worker-bound wait produce the same outcomes as the built-in ones.
template <typename t_type>
class worker_wait_result final {
public:
    worker_wait_result(const worker_wait_result&) = default;
    worker_wait_result(worker_wait_result&&) = default;
    worker_wait_result& operator=(const worker_wait_result&) = delete;
    worker_wait_result& operator=(worker_wait_result&&) = delete;

    [[nodiscard]] worker_wait_status status() const noexcept {
        return std::holds_alternative<detail::worker_wait_value<t_type>>(result_)
                   ? worker_wait_status::value
                   : std::get<worker_wait_status>(result_);
    }

    [[nodiscard]] bool has_value() const noexcept {
        return std::holds_alternative<detail::worker_wait_value<t_type>>(result_);
    }

    [[nodiscard]] const t_type& value() const& {
        const auto* result_value = std::get_if<detail::worker_wait_value<t_type>>(&result_);
        if (result_value == nullptr) {
            throw std::logic_error("worker wait result has no value");
        }
        return result_value->value_;
    }
    const t_type& value() const&& = delete;

    [[nodiscard]] t_type& value() & {
        auto* result_value = std::get_if<detail::worker_wait_value<t_type>>(&result_);
        if (result_value == nullptr) {
            throw std::logic_error("worker wait result has no value");
        }
        return result_value->value_;
    }
    t_type& value() && = delete;

    [[nodiscard]] t_type take_value() && {
        auto* result_value = std::get_if<detail::worker_wait_value<t_type>>(&result_);
        if (result_value == nullptr) {
            throw std::logic_error("worker wait result has no value");
        }
        return std::move(result_value->value_);
    }

    template <typename u_type>
        requires std::constructible_from<t_type, u_type&&>
    [[nodiscard]] static worker_wait_result with_value(u_type&& value) noexcept(
        std::is_nothrow_constructible_v<t_type, u_type&&>) {
        return worker_wait_result(std::in_place_type<detail::worker_wait_value<t_type>>, std::forward<u_type>(value));
    }

    // Every status except value; value outcomes come only from with_value().
    [[nodiscard]] static worker_wait_result with_status(worker_wait_status status) {
        if (status == worker_wait_status::value) {
            throw std::invalid_argument("worker wait status result requires a non-value status");
        }
        return worker_wait_result(status);
    }

private:
    using result_type = std::variant<detail::worker_wait_value<t_type>, worker_wait_status>;

    template <typename u_type>
    explicit worker_wait_result(
        std::in_place_type_t<detail::worker_wait_value<t_type>>, u_type&& value) noexcept(std::is_nothrow_constructible_v<t_type, u_type&&>)
        : result_(std::in_place_type<detail::worker_wait_value<t_type>>, std::forward<u_type>(value)) {}

    explicit worker_wait_result(worker_wait_status status) noexcept
        : result_(status) {}

    result_type result_;
};

// The outcome of a bounded delivery: post, submit, send, complete, or start.
// The category is fixed at construction. A rejection owns the unconsumed
// payload; when accepted_type is not void, acceptance owns that value (the
// started operation). take_rejected() and take_value() both consume their
// payload. Only the delivering primitive produces results, so an accepted
// result always reflects a real admission.
template <typename t_type, typename status_type, status_type accepted_status, typename accepted_type = void>
class worker_delivery_result final {
    using accepted_storage_type = std::conditional_t<std::is_void_v<accepted_type>, std::monostate, accepted_type>;

public:
    worker_delivery_result(const worker_delivery_result&) = delete;
    worker_delivery_result& operator=(const worker_delivery_result&) = delete;
    worker_delivery_result(worker_delivery_result&&) noexcept(
        std::is_nothrow_move_constructible_v<t_type> && std::is_nothrow_move_constructible_v<accepted_storage_type>) =
        default;
    worker_delivery_result& operator=(worker_delivery_result&&) = delete;

    [[nodiscard]] status_type status() const noexcept {
        return status_;
    }

    [[nodiscard]] bool accepted() const noexcept {
        return status_ == accepted_status;
    }

    template <typename self_type = accepted_type>
        requires(!std::is_void_v<self_type>)
    [[nodiscard]] self_type& value() & {
        return require_accepted();
    }
    template <typename self_type = accepted_type>
        requires(!std::is_void_v<self_type>)
    [[nodiscard]] const self_type& value() const& {
        return const_cast<worker_delivery_result&>(*this).require_accepted();
    }
    template <typename self_type = accepted_type>
        requires(!std::is_void_v<self_type>)
    self_type& value() && = delete;
    template <typename self_type = accepted_type>
        requires(!std::is_void_v<self_type>)
    const self_type& value() const&& = delete;

    template <typename self_type = accepted_type>
        requires(!std::is_void_v<self_type>)
    [[nodiscard]] self_type take_value() && {
        auto& accepted_value = require_accepted();
        struct clear_accepted final {
            std::optional<accepted_storage_type>& accepted_;
            ~clear_accepted() {
                accepted_.reset();
            }
        } clear{accepted_};
        return std::move(accepted_value);
    }

    [[nodiscard]] t_type* rejected() & noexcept {
        return rejected_ ? std::addressof(*rejected_) : nullptr;
    }

    [[nodiscard]] const t_type* rejected() const& noexcept {
        return rejected_ ? std::addressof(*rejected_) : nullptr;
    }

    t_type* rejected() && = delete;
    const t_type* rejected() const&& = delete;

    // A failed extraction retains the payload, in the state its move left it.
    // Only successful return-value construction consumes the rejected value.
    [[nodiscard]] t_type take_rejected() && {
        if (status_ == accepted_status || !rejected_) {
            throw std::logic_error("delivery result has no rejected value");
        }
        struct clear_rejected final {
            std::optional<t_type>& rejected_;
            const int exceptions_{std::is_nothrow_move_constructible_v<t_type> ? 0 : std::uncaught_exceptions()};
            ~clear_rejected() {
                if (std::is_nothrow_move_constructible_v<t_type> || std::uncaught_exceptions() == exceptions_) {
                    rejected_.reset();
                }
            }
        } clear{rejected_};
        return std::move(*rejected_);
    }

    friend bool operator==(const worker_delivery_result& outcome, status_type status) noexcept {
        return outcome.status_ == status;
    }
    friend bool operator==(status_type status, const worker_delivery_result& outcome) noexcept {
        return outcome == status;
    }

private:
    friend struct detail::worker_delivery_access;

    explicit worker_delivery_result(status_type status) noexcept
        : status_(status) {}

    worker_delivery_result(status_type status, std::in_place_t, accepted_storage_type&& accepted_value) noexcept(
        std::is_nothrow_move_constructible_v<accepted_storage_type>)
        : status_(status),
          accepted_(std::move(accepted_value)) {}

    worker_delivery_result(status_type status, t_type&& rejected) noexcept(
        std::is_nothrow_move_constructible_v<t_type>)
        : status_(status),
          rejected_(std::move(rejected)) {}

    [[nodiscard]] accepted_storage_type& require_accepted() {
        if (status_ != accepted_status || !accepted_) {
            throw std::logic_error("delivery result has no accepted value");
        }
        return *accepted_;
    }

    status_type status_;
    std::optional<t_type> rejected_;
    [[no_unique_address]] std::conditional_t<std::is_void_v<accepted_type>, std::monostate,
        std::optional<accepted_storage_type>>
        accepted_;
};

namespace detail {

// The producer side of worker_delivery_result, reserved to the delivering
// primitives so callers cannot fabricate an accepted outcome.
struct worker_delivery_access final {
    template <typename result_type>
    [[nodiscard]] static result_type accept() noexcept {
        return result_type(accepted_status_of<result_type>());
    }

    template <typename result_type, typename accepted_value_type>
    [[nodiscard]] static result_type accept(accepted_value_type&& accepted_value) noexcept(
        std::is_nothrow_move_constructible_v<std::remove_cvref_t<accepted_value_type>>) {
        return result_type(
            accepted_status_of<result_type>(), std::in_place, std::forward<accepted_value_type>(accepted_value));
    }

    template <typename result_type, typename status_type, typename rejected_type>
    [[nodiscard]] static result_type reject(status_type status, rejected_type&& value) {
        if (status == accepted_status_of<result_type>()) {
            throw std::invalid_argument("rejected delivery result requires a rejection status");
        }
        if constexpr (is_move_only_function<std::remove_cvref_t<rejected_type>>) {
            if (!value) {
                throw std::invalid_argument("rejected delivery result requires a callable task");
            }
        }
        return result_type(status, std::forward<rejected_type>(value));
    }

private:
    template <typename result_type>
    struct accepted_status_traits;

    template <typename t_type, typename status_type, status_type accepted_status, typename accepted_type>
    struct accepted_status_traits<worker_delivery_result<t_type, status_type, accepted_status, accepted_type>> final {
        static constexpr status_type value = accepted_status;
    };

    template <typename result_type>
    [[nodiscard]] static constexpr auto accepted_status_of() noexcept {
        return accepted_status_traits<result_type>::value;
    }
};

}  // namespace detail

}  // namespace ruvia

namespace ruvia {

using worker_id_type = std::uint64_t;

[[nodiscard]] constexpr std::uint64_t worker_selection_hash(std::string_view key) noexcept {
    return fnv1a_accumulate(fnv1a_offset_basis, key);
}

enum class post_status : std::uint8_t {
    accepted,
    queue_full,
    worker_stopping,
};

template <typename signature_type = void()>
using post_outcome =
    worker_delivery_result<move_only_function<signature_type>, post_status, post_status::accepted>;
using post_result_type = post_outcome<void()>;

class blocking_pool;

namespace detail {
class worker_dispatcher;
struct worker_ref_access;

// Per-owner-type phase table; a null entry skips that phase.
struct worker_shutdown_operations_type final {
    using phase_type = void (*)(void* owner) noexcept;

    phase_type stopping_;
    phase_type stopping_complete_;
};
}  // namespace detail

// An owner observes its worker's shutdown through `void worker_stopping()
// noexcept`, run in the first notification pass, and/or `void
// worker_stopping_complete() noexcept`, run in the second pass after every
// registered owner has seen the first.
template <typename owner_type>
concept worker_shutdown_observer =
    requires(owner_type& owner) {
        { owner.worker_stopping() } noexcept;
    } || requires(owner_type& owner) {
        { owner.worker_stopping_complete() } noexcept;
    };

namespace detail {

template <typename owner_type>
[[nodiscard]] consteval worker_shutdown_operations_type make_worker_shutdown_operations() noexcept {
    worker_shutdown_operations_type operations{nullptr, nullptr};
    if constexpr (requires(owner_type& owner) {
                      { owner.worker_stopping() } noexcept;
                  }) {
        operations.stopping_ = [](void* owner) noexcept { static_cast<owner_type*>(owner)->worker_stopping(); };
    }
    if constexpr (requires(owner_type& owner) {
                      { owner.worker_stopping_complete() } noexcept;
                  }) {
        operations.stopping_complete_ = [](void* owner) noexcept {
            static_cast<owner_type*>(owner)->worker_stopping_complete();
        };
    }
    return operations;
}

template <typename owner_type>
inline constexpr worker_shutdown_operations_type worker_shutdown_operations =
    make_worker_shutdown_operations<owner_type>();

}  // namespace detail

enum class worker_registration_status : std::uint8_t {
    registered,
    worker_stopping,
};

// Intrusive link of one shared owner into its worker's shutdown registry,
// held as a member of that owner (see worker_ref::register_shutdown).
// Linking and unlinking are O(1) under the registry mutex and never allocate.
// The link keeps the registry alive, so destruction unlinks even after the
// owner released its own worker_ref, on any thread. Declare it as the
// owner's first member so it unlinks only after every other member has
// been destroyed and no phase can observe a half-destroyed owner.
class worker_shutdown_registration final {
public:
    worker_shutdown_registration() noexcept = default;
    worker_shutdown_registration(const worker_shutdown_registration&) = delete;
    worker_shutdown_registration& operator=(const worker_shutdown_registration&) = delete;
    worker_shutdown_registration(worker_shutdown_registration&&) = delete;
    worker_shutdown_registration& operator=(worker_shutdown_registration&&) = delete;
    ~worker_shutdown_registration();

private:
    friend class detail::worker_dispatcher;

    const detail::worker_shutdown_operations_type* operations_{nullptr};
    std::shared_ptr<detail::worker_dispatcher> registry_;
    // Aliases the owner object; locking it pins the owner, and therefore this
    // registration, for the duration of one phase callback.
    std::weak_ptr<void> owner_;
    worker_shutdown_registration* previous_{nullptr};
    worker_shutdown_registration* next_{nullptr};
    bool linked_{false};
};

class worker_ref final {
public:
    worker_ref() noexcept = default;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] bool accepting() const noexcept;
    [[nodiscard]] bool is_current() const noexcept;
    [[nodiscard]] worker_id_type id() const noexcept;
    // The blocking pool this worker's event loop borrows, or null; run_blocking()
    // on a worker without one reports blocking_status::unavailable. The pool's
    // owner keeps it alive for at least the worker's lifetime.
    [[nodiscard]] ruvia::blocking_pool* blocking_pool() const noexcept;

    // Native I/O integration, not bounded business admission. The executor
    // borrows the runtime's io_context: finish its operations before the
    // runtime/attachment retires and never retain it beyond context destruction.
    // Empty or detached worker refs throw std::logic_error; stopping workers still
    // expose their executor so already admitted operations can finish.
    [[nodiscard]] asio::io_context::executor_type executor() const;

    // Worker-affine timer registration. This stable worker_ref must outlive the
    // borrowed registration; destruction unregisters without a late callback.
    // Returns worker_stopping without storing the completion once this worker's
    // timer queue has stopped; callers that resume work from the completion
    // must handle it. Rescheduling a still-pending registration throws
    // std::logic_error.
    worker_timer_schedule_status schedule_timer(worker_timer_registration& registration,
        std::chrono::steady_clock::time_point deadline,
        move_only_function<void(worker_timer_outcome)> completion) const&;
    worker_timer_schedule_status schedule_timer(worker_timer_registration&,
        std::chrono::steady_clock::time_point,
        move_only_function<void(worker_timer_outcome)>) const&& = delete;

    // Reserves bounded admission before synchronously invoking the factory, so
    // ownership transfers only after admission. Keeps the selected endpoint
    // alive across user factory code, even if it reentrantly retires the runtime
    // and clears this worker_ref. accepted does not guarantee execution: detaching
    // the runtime abandons an admitted callback and releases its owned inputs.
    [[nodiscard]] post_status post_factory(move_only_function<move_only_function<void()>()> factory) const;

    // post_factory() for a typed payload: `wrap` turns the payload into the
    // posted callable only after admission, and a refusal returns the
    // untouched payload in the result. An empty worker_ref refuses with
    // worker_stopping.
    template <typename payload_type, typename wrap_type>
        requires std::invocable<wrap_type&, payload_type&&> &&
                 std::convertible_to<std::invoke_result_t<wrap_type&, payload_type&&>, move_only_function<void()>>
    [[nodiscard]] worker_delivery_result<payload_type, post_status, post_status::accepted> post_payload(
        payload_type payload, wrap_type wrap) const {
        using result_type = worker_delivery_result<payload_type, post_status, post_status::accepted>;
        const auto status = post_factory([&]() -> move_only_function<void()> { return wrap(std::move(payload)); });
        if (status != post_status::accepted) {
            return detail::worker_delivery_access::reject<result_type>(status, std::move(payload));
        }
        return detail::worker_delivery_access::accept<result_type>();
    }

    template <typename fn_type>
        requires detail::move_only_function_target<void, fn_type>
    [[nodiscard]] post_result_type post(fn_type&& fn) const {
        if constexpr (detail::move_only_function_borrow_safe_input<void(), fn_type>) {
            return post_task(move_only_function<void()>(std::forward<fn_type>(fn)));
        } else {
            // Snapshot the endpoint before invoking any user-controlled construction/move.
            const worker_ref snapshot = *this;
            return snapshot.post_task(move_only_function<void()>(std::forward<fn_type>(fn)));
        }
    }

    // Resumes `continuation` on this worker in a later turn, never inline --
    // for a coroutine suspended in an operation this worker already runs.
    // Requests made from one thread resume in the order they were made; there
    // is no order between requests made from different threads (on a worker
    // with a single owner, the worker's own requests reach the event loop only
    // after its current handler returns). Unlike post() it cannot be refused by
    // a full queue; on the worker's own thread it goes straight to the event
    // loop without a type-erased callable. Requested from another thread once
    // the worker's execution context has been detached, or on an empty worker_ref,
    // it terminates. A request the event loop is destroyed before
    // running is dropped and the coroutine never resumes, so the worker must
    // run until every operation it admitted has completed, as for any started
    // task.
    void defer_resume(std::coroutine_handle<> continuation) const noexcept;

    // The completion lane of an operation this worker already admitted:
    // cancellation, cleanup, or result delivery. It takes no retirement
    // protection of its own; loop-level cleanup that belongs to no admitted
    // operation uses event_loop::defer_cleanup() instead. Unlike post() a full queue
    // never refuses it; on the worker's own thread it runs in a later turn,
    // never inline. Returns false, destroying `completion` on the caller's
    // thread, once the worker's execution context has been detached (or on an
    // empty worker_ref): the admitted operation has then retired with
    // its worker. Throws std::invalid_argument for an empty callable.
    [[nodiscard]] bool defer_completion(move_only_function<void()> completion) const;

    // Links `owner->*registration` into this worker's shutdown registry, so
    // the worker's stop reaches the owner's worker_stopping() and
    // worker_stopping_complete() phases on the stopping thread. The registry
    // holds only a weak reference: an owner already being destroyed is
    // skipped, and destroying the registration unlinks it. Returns
    // worker_stopping, linking nothing, once the worker no longer accepts
    // work (or on an empty worker_ref). Linking an already registered
    // member throws std::logic_error; a null owner or member throws
    // std::invalid_argument.
    template <typename owner_type>
        requires worker_shutdown_observer<owner_type>
    [[nodiscard]] worker_registration_status register_shutdown(
        const std::shared_ptr<owner_type>& owner, worker_shutdown_registration owner_type::* registration) const {
        if (owner == nullptr || registration == nullptr) {
            throw std::invalid_argument("worker shutdown registration requires an owner member");
        }
        return link_shutdown((*owner).*registration, std::weak_ptr<void>(owner),
            detail::worker_shutdown_operations<owner_type>);
    }

private:
    explicit worker_ref(std::shared_ptr<detail::worker_dispatcher> dispatcher) noexcept;
    [[nodiscard]] post_result_type post_task(move_only_function<void()> task) const;
    [[nodiscard]] worker_registration_status link_shutdown(worker_shutdown_registration& registration,
        std::weak_ptr<void> owner, const detail::worker_shutdown_operations_type& operations) const;

    // Owns the stable dispatcher endpoint, not the worker's io_context.
    // The worker detaches that endpoint before destroying its execution context.
    std::shared_ptr<detail::worker_dispatcher> dispatcher_;
    friend struct detail::worker_ref_access;
};

namespace detail {

struct worker_ref_access {
    [[nodiscard]] static worker_ref make(
        const std::shared_ptr<worker_dispatcher>& dispatcher) noexcept;
    static void defer_or_terminate(
        const worker_ref& worker, move_only_function<void()> task) noexcept;
    static void when_shutdown_notifications_complete(
        const worker_ref& worker, move_only_function<void()> callback);
    static void when_idle(const worker_ref& worker, move_only_function<void()> callback);
    static void wait_for_reservations(const worker_ref& worker) noexcept;
    [[nodiscard]] static bool timers_stopping(const worker_ref& worker) noexcept;
    static void enable_external_runner(const worker_ref& worker) noexcept;
    // The worker whose event loop runs on the calling thread; empty elsewhere.
    [[nodiscard]] static worker_ref current() noexcept;
};

}  // namespace detail

}  // namespace ruvia

namespace ruvia {

enum class worker_runtime_state : std::uint8_t {
    ready,
    running,
    stopping,
    stopped,
};

class blocking_pool;
class event_loop;
class worker_group;
class worker_runtime_context;

namespace detail {
struct event_loop_state;
}

enum class worker_io_policy : std::uint8_t { synchronized,
    single_owner };

struct worker_runtime_options final {
    std::size_t queue_capacity_{4096};
    worker_io_policy io_policy_{worker_io_policy::synchronized};
    // Borrowed pool for run_blocking() on this worker; it must outlive the
    // runtime. Null leaves the worker without one.
    blocking_pool* blocking_pool_{nullptr};
};

struct worker_runtime_hooks final {
    // Owner thread, once, at launch. Begins startup; the owner publishes
    // readiness with mark_ready() once its (possibly asynchronous) startup work
    // has completed.
    move_only_function<void()> startup_{};
    // Owner thread, once, after request_serve() follows readiness. Serving is
    // published when it returns; a throw fails the runtime.
    move_only_function<void()> serve_{};
    // Runs on the owner after dispatcher admission and shutdown notifications
    // close. A custom policy must eventually call finalize(), after its external
    // producers are quiescent. Without a policy, stopping finalizes immediately.
    move_only_function<void()> stop_admission_{};
    // Owner thread, once, when finalization releases the work guard. Retires
    // domain resources; pending I/O and continuations still drain afterwards.
    // During finalization, its target's move and destruction may reenter finalize().
    move_only_function<void()> finalize_{};
    // Runs once, for the first failure only, on the thread that reported it:
    // the owner for escaped failures, the caller for fail().
    move_only_function<void(std::exception_ptr)> failure_{};
    // Terminal owner-thread cleanup, after I/O and completion handlers drain.
    move_only_function<void()> shutdown_{};
};

// The unique owner of a worker's thread, io_context, dispatcher, and generic
// startup/serve/stop/drain/join lifecycle, including its first failure.
// Protocol/application state is composed via hooks; it must remain alive
// through join(). No hook may throw except startup_ and serve_.
class worker_runtime final {
    class impl_type;

public:
    explicit worker_runtime(worker_runtime_options options = {});
    ~worker_runtime();

    worker_runtime(const worker_runtime&) = delete;
    worker_runtime& operator=(const worker_runtime&) = delete;
    worker_runtime(worker_runtime&&) = delete;
    worker_runtime& operator=(worker_runtime&&) = delete;

    // Configure once, before publishing the runtime or starting/stopping it.
    // Hook target moves may call take_failure() during configuration.
    void configure(worker_runtime_hooks hooks);
    // Launches the owner thread. Returns false when a stop already closed the
    // runtime before launch; a second start throws std::logic_error.
    [[nodiscard]] bool start();
    // Two-phase startup barrier. Owner-published readiness; a stop first aborts
    // it. wait_until_ready() returns false once stopped before readiness and
    // rethrows the first failure. request_serve() requires readiness and runs
    // serve_ on the owner; wait_until_serving() mirrors wait_until_ready().
    void mark_ready() noexcept;
    [[nodiscard]] bool wait_until_ready();
    void request_serve();
    [[nodiscard]] bool wait_until_serving();
    // Every call closes dispatcher admission before returning. Repeated or
    // reentrant requests do not wait for the winning stop dispatch to finish.
    void request_stop() noexcept;
    // Records the first failure, wakes phase waiters, and requests stop. Only
    // the winning failure reaches the failure_ hook. Returns true for it.
    bool fail(std::exception_ptr failure) noexcept;
    // Cold-path lifecycle control, serialized with finalization. Returns false
    // once stopping starts; it cannot strand a control after the thread barrier.
    [[nodiscard]] bool post_control(move_only_function<void()> control) noexcept;
    // A reliable owner-thread phase-two control; the first call wins. The
    // finalize_ hook retires domain resources before the work guard is
    // released. Pending I/O/continuations still drain; the io_context is never
    // stopped.
    void finalize() noexcept;
    // Establishes the thread barrier, including owner-affine draining when
    // stopped before start. Concurrent joins wait for the same barrier. This
    // does not rethrow run failures: inspect/rethrow them after all owners join.
    // Waits for any external stop dispatch before draining a cold owner. Joining
    // from its owner or from a thread currently dispatching its stop hooks
    // throws logic_error instead of waiting on itself.
    void join();
#if defined(_WIN32)
    class thread_join_operation final {
    public:
        thread_join_operation(const thread_join_operation&) = delete;
        thread_join_operation& operator=(const thread_join_operation&) = delete;
        void operator&() const = delete;
        ~thread_join_operation();
        [[nodiscard]] bool await_ready();
        [[nodiscard]] bool await_suspend(std::coroutine_handle<> continuation);
        [[nodiscard]] std::uintptr_t await_resume();

    private:
        thread_join_operation(worker_runtime& owner, worker_ref caller) noexcept;
        worker_runtime* owner_;
        worker_ref caller_;
        thread_join_operation* next_{};
        std::coroutine_handle<> continuation_{};
        bool started_{};
        bool resumed_{};
        friend class worker_runtime;
        friend class impl_type;
    };
    // Await on caller: requests stop and joins any external stop dispatch
    // without blocking its event loop. Launches a cold owner's drain thread
    // if needed, then returns an owned SYNCHRONIZE HANDLE as uintptr_t.
    // Wait asynchronously for that HANDLE, close it, and call join() to reap
    // the exited thread. Zero means already joined. Runtime must outlive the
    // operation; concurrent join() is not permitted.
    [[nodiscard]] thread_join_operation prepare_thread_join(worker_ref caller) &;
    thread_join_operation prepare_thread_join(worker_ref) && = delete;
#endif
    // Takes reporting ownership of the first failure, if any: destruction no
    // longer reports a failure taken here. Later calls return the same failure.
    [[nodiscard]] std::exception_ptr take_failure() noexcept;
    [[nodiscard]] worker_runtime_state state() const noexcept;
    [[nodiscard]] asio::io_context& io_context() & noexcept;
    asio::io_context& io_context() && = delete;
    [[nodiscard]] const worker_ref& worker() const& noexcept;
    const worker_ref& worker() const&& = delete;
    // The event loop over this runtime's worker, created on first use, for
    // try_start()/start(), on_stop(), defer_cleanup() and report_failure()
    // (which fails this runtime). Once a loop exists, the runtime's stop
    // admission waits for the loop's root tasks, stop callbacks, and cleanup
    // to drain before stop_admission_ (or the default finalize) runs. The loop
    // borrows this runtime: no copy of it may outlive the runtime.
    [[nodiscard]] event_loop loop() &;
    event_loop loop() && = delete;

private:
    friend class worker_group;
    friend struct detail::event_loop_state;
    [[nodiscard]] worker_runtime_context& context() & noexcept;
    // The first failure without taking ownership of it, for comparisons.
    [[nodiscard]] std::exception_ptr recorded_failure() const noexcept;
    void bind_group(worker_group& group);
    void unbind_group(worker_group& group) noexcept;
    // Owner thread: the loop's retirement gate has drained; continue the stop
    // admission the gate deferred.
    void finish_loop_stop_admission() noexcept;

    std::unique_ptr<impl_type> impl_;
};

}  // namespace ruvia

namespace ruvia {

class worker_runtime;

// Sequences a set of worker_runtime owners through one lifecycle:
// - start(): launches every member and waits until all are ready;
// - serve(): requests serving on every member and waits until all are serving;
// - request_stop(): closes admission on every member at once; lifecycle hooks
//   may query stop_requested() or repeat request_stop();
// - join(): requests stop, waits for stop dispatch, and joins every member.
// Member failure closes admission on the entire group and wakes pending phase
// waits, preserving the original exception. An independently stopped member
// aborts the group; a group stop instead makes its phase calls return false.
// The group borrows its members, which must outlive it. Its destruction removes
// and synchronizes member observations; callers still join the group explicitly
// to retire its members. All calls except request_stop() and stop_requested()
// belong to one lifecycle thread.
class worker_group final {
public:
    worker_group();
    ~worker_group();

    worker_group(const worker_group&) = delete;
    worker_group& operator=(const worker_group&) = delete;

    // Cold setup before start() or stop. Each member belongs to one group and
    // may be added once.
    void add(worker_runtime& member);
    // Launches every member without waiting for readiness; a thread creation
    // failure stops and joins the group before rethrowing.
    void launch();
    // Phase one. Returns false when the group was stopped first; rethrows the
    // first member failure; throws std::runtime_error when a member stopped on
    // its own before readiness.
    [[nodiscard]] bool start();
    // Phase two, after start() succeeded. Same outcome rules as start().
    [[nodiscard]] bool serve();
    void request_stop() noexcept;
    [[nodiscard]] bool stop_requested() const noexcept;
    // Requests stop and waits for its member notifications to finish before
    // joining every member. Returns the failure its caller rethrows:
    // `primary` when given (a failure the caller already holds, such as one
    // start() or serve() threw), else the first member failure in registration
    // order. It is chosen before any report, so it is never reported here;
    // every other member failure is reported as unhandled ("additional worker"),
    // equivalent ones collapsed into one line with their count. Repeated joins
    // return the first result without repeating retirement or diagnostics.
    [[nodiscard]] std::exception_ptr join(std::exception_ptr primary = nullptr) noexcept;

private:
    friend class worker_runtime;
    [[nodiscard]] bool launch(worker_runtime& member);
    [[nodiscard]] bool settle(bool reached, const char* phase);
    void member_failed(worker_runtime& member, std::exception_ptr failure) noexcept;
    void member_stopping() noexcept;
    void wait_for_stop_dispatch() noexcept;
    [[nodiscard]] std::exception_ptr observe_phase_failure() noexcept;
    [[noreturn]] void rethrow_phase_failure(std::exception_ptr fallback);

    // Borrowed members in registration order.
    std::vector<worker_runtime*> members_;
    mutable std::mutex mutex_;
    std::condition_variable stop_changed_;
    bool stopping_{false};
    bool stop_dispatching_{false};
    bool sealed_{false};
    bool ready_{false};
    bool serve_requested_{false};
    bool member_stopped_{false};
    std::exception_ptr failure_;
    worker_runtime* failed_member_{nullptr};
    bool joining_{false};
    bool joined_{false};
    std::exception_ptr join_failure_;
};

}  // namespace ruvia

namespace ruvia {

// Owns only a detachable owner pointer and a stable worker_ref; queued work
// retains the target so that releasing it never depends on the owner's lifetime.
template <typename owner_type>
class worker_cancellation_target;

template <typename owner_type>
[[nodiscard]] inline std::shared_ptr<worker_cancellation_target<owner_type>>
make_worker_cancellation_target(owner_type& owner_value, const worker_ref& worker_value);

template <typename owner_type>
class worker_cancellation_target final
    : public std::enable_shared_from_this<worker_cancellation_target<owner_type>> {
private:
    struct constructor_token final {};

public:
    worker_cancellation_target(
        constructor_token, owner_type& owner_value, const worker_ref& worker_value) noexcept
        : owner_(&owner_value),
          worker_(worker_value) {}
    worker_cancellation_target(const worker_cancellation_target&) = delete;
    worker_cancellation_target& operator=(const worker_cancellation_target&) = delete;
    worker_cancellation_target(worker_cancellation_target&&) = delete;
    worker_cancellation_target& operator=(worker_cancellation_target&&) = delete;
    [[nodiscard]] const worker_ref& worker() const noexcept {
        return worker_;
    }
    [[nodiscard]] std::uint64_t next_operation_id() noexcept {
        if (!worker_.is_current()) {
            std::terminate();
        }
        if (++next_operation_id_ == 0) {
            ++next_operation_id_;
        }
        return next_operation_id_;
    }
    [[nodiscard]] std::shared_ptr<worker_cancellation_target> retain() noexcept {
        auto target = this->weak_from_this().lock();
        if (!target) {
            std::terminate();
        }
        return target;
    }
    void dispatch(std::uint64_t operation_id) noexcept {
        if (auto* owner = owner_.load(std::memory_order_acquire)) {
            owner->cancel_operation_by_id(operation_id);
        }
    }
    void detach(owner_type& owner_value) noexcept {
        auto* previous = owner_.exchange(nullptr, std::memory_order_acq_rel);
        if (previous && previous != &owner_value) {
            std::terminate();
        }
    }

private:
    friend std::shared_ptr<worker_cancellation_target<owner_type>>
    make_worker_cancellation_target<owner_type>(owner_type&, const worker_ref&);

    // Construction is restricted to make_worker_cancellation_target via constructor_token.
    std::atomic<owner_type*> owner_;
    worker_ref worker_;
    std::uint64_t next_operation_id_{0};
};

template <typename owner_type>
[[nodiscard]] inline std::shared_ptr<worker_cancellation_target<owner_type>>
make_worker_cancellation_target(owner_type& owner_value, const worker_ref& worker_value) {
    using target_type = worker_cancellation_target<owner_type>;
    return std::make_shared<target_type>(
        typename target_type::constructor_token{}, owner_value, worker_value);
}

template <typename target_type>
class worker_cancellation_dispatch final {
public:
    worker_cancellation_dispatch(std::shared_ptr<target_type> target, std::uint64_t operation_id) noexcept
        : target_(std::move(target)),
          operation_id_(operation_id) {}
    void operator()() noexcept {
        target_->dispatch(operation_id_);
    }

private:
    std::shared_ptr<target_type> target_;
    std::uint64_t operation_id_;
};

template <typename target_type>
class worker_cancellation_post final {
public:
    worker_cancellation_post(target_type* target, std::uint64_t operation_id) noexcept
        : target_(target),
          operation_id_(operation_id) {
        if (!target_) {
            std::terminate();
        }
    }
    void operator()() noexcept {
        if (target_->worker().is_current()) {
            target_->dispatch(operation_id_);
            return;
        }
        auto retained = target_->retain();
        // Cancellation belongs to an already admitted operation. Use the
        // dispatcher completion lane so a full user queue cannot reject it.
        // Detached endpoints have already retired their owners and operations.
        (void)retained->worker().defer_completion(worker_cancellation_dispatch<target_type>(retained, operation_id_));
    }

private:
    target_type* target_;
    std::uint64_t operation_id_;
};

template <typename target_type>
inline constexpr bool worker_cancellation_post_is_inline =
    detail::move_only_function_fits_inline<worker_cancellation_post<target_type>> &&
    detail::move_only_function_fits_inline<worker_cancellation_dispatch<target_type>>;

// Worker-affine registration owns the supplied identity until reset(). Bind
// owner state before arm(), which may deliver an already-requested stop inline.
// reset() joins the callback before clearing its identity; a late posted
// cancellation cannot address a reused slot.
// The target is borrowed without retaining it; its owner must keep it alive
// until reset() has joined the callback.
template <typename target_type>
class worker_cancellation_registration final {
    static_assert(worker_cancellation_post_is_inline<target_type>);

public:
    worker_cancellation_registration(const std::shared_ptr<target_type>& target, std::uint64_t& identity)
        : target_(target.get()),
          identity_(&identity),
          id_(require_target(target).next_operation_id()) {
        identity = id_;
    }
    worker_cancellation_registration(std::shared_ptr<target_type>&&, std::uint64_t&) = delete;
    worker_cancellation_registration(const std::shared_ptr<target_type>&&, std::uint64_t&) = delete;
    worker_cancellation_registration(const worker_cancellation_registration&) = delete;
    worker_cancellation_registration& operator=(const worker_cancellation_registration&) = delete;
    worker_cancellation_registration(worker_cancellation_registration&&) = delete;
    worker_cancellation_registration& operator=(worker_cancellation_registration&&) = delete;
    ~worker_cancellation_registration() {
        reset();
    }
    [[nodiscard]] std::uint64_t id() const noexcept {
        return id_;
    }
    void arm(const stop_token& token) {
        if (!target_ || armed_) {
            std::terminate();
        }
        armed_ = true;
        if (token.stoppable()) {
            // stop_token delivers an already-requested stop inline and invokes
            // a linked callback at most once. A second stop_requested dispatch
            // would duplicate cancellation and race a foreign queued delivery.
            token.register_callback(registration_, worker_cancellation_post<target_type>(target_, id_));
        }
    }
    void reset() noexcept {
        registration_.reset();
        if (identity_ && *identity_ == id_) {
            *identity_ = 0;
        }
        identity_ = nullptr;
        target_ = nullptr;
    }

private:
    static target_type& require_target(const std::shared_ptr<target_type>& target) {
        if (!target || !target->worker().is_current()) {
            std::terminate();
        }
        return *target;
    }
    target_type* target_;
    std::uint64_t* identity_{};
    std::uint64_t id_;
    bool armed_{};
    stop_registration registration_;
};

}  // namespace ruvia

namespace ruvia::detail {

struct suspend_race_pending final {};

// A completion can race the suspension of the awaiter's coroutine in two
// orders: complete-then-suspend and suspend-then-complete. A caller that
// initiates an asynchronous operation must not resume the continuation from
// inside await_suspend before that coroutine is actually suspended; driving
// both orderings through this state machine makes the await_suspend return
// value (suspend or not) the single verdict on which ordering happened.
//
// Payload moves may throw; complete() then restores the state it found so the
// awaiter can retry or propagate the failure from the original call site.
// Calls must be serialized by the owner, either on one thread or under its lock.
template <typename t_type>
class suspend_race_state final {
public:
    suspend_race_state() = default;
    suspend_race_state(const suspend_race_state&) = delete;
    suspend_race_state& operator=(const suspend_race_state&) = delete;
    suspend_race_state(suspend_race_state&&) = delete;
    suspend_race_state& operator=(suspend_race_state&&) = delete;

    // Called from await_suspend after the operation was initiated: publishes
    // the continuation when still pending, or reports that the operation
    // already completed synchronously (its result is consumed by take_value()).
    [[nodiscard]] bool suspend(std::coroutine_handle<> continuation) noexcept {
        if (std::holds_alternative<suspend_race_pending>(state_)) {
            state_.template emplace<suspend_race_suspended_type>(continuation);
            return true;
        }
        if (std::holds_alternative<suspend_race_ready_before_suspend_type>(state_)) {
            return false;
        }
        std::terminate();
    }

    // Returns true only when the suspended coroutine now needs an explicit
    // wake. A completion racing before await_suspend is consumed there instead.
    [[nodiscard]] bool complete(t_type&& value) {
        if (std::holds_alternative<suspend_race_pending>(state_)) {
            try {
                state_.template emplace<suspend_race_ready_before_suspend_type>(std::move(value));
            } catch (...) {
                state_.template emplace<suspend_race_pending>();
                throw;
            }
            return false;
        }
        if (auto* suspended = std::get_if<suspend_race_suspended_type>(&state_)) {
            const auto continuation = suspended->continuation();
            try {
                state_.template emplace<suspend_race_ready_after_suspend_type>(
                    std::move(value), continuation);
            } catch (...) {
                state_.template emplace<suspend_race_suspended_type>(continuation);
                throw;
            }
            return true;
        }
        std::terminate();
    }

    [[nodiscard]] std::coroutine_handle<> continuation() const noexcept {
        const auto* ready = std::get_if<suspend_race_ready_after_suspend_type>(&state_);
        if (ready == nullptr) {
            std::terminate();
        }
        return ready->continuation();
    }

    [[nodiscard]] t_type take_value() noexcept(std::is_nothrow_move_constructible_v<t_type>) {
        if (auto* ready = std::get_if<suspend_race_ready_before_suspend_type>(&state_)) {
            return std::move(*ready).take_value();
        }
        if (auto* ready = std::get_if<suspend_race_ready_after_suspend_type>(&state_)) {
            return std::move(*ready).take_value();
        }
        std::terminate();
    }

private:
    class suspend_race_suspended_type final {
    public:
        explicit suspend_race_suspended_type(std::coroutine_handle<> continuation) noexcept
            : continuation_(continuation) {}

        [[nodiscard]] std::coroutine_handle<> continuation() const noexcept {
            return continuation_;
        }

    private:
        std::coroutine_handle<> continuation_;
    };

    class suspend_race_ready_before_suspend_type final {
    public:
        explicit suspend_race_ready_before_suspend_type(t_type&& value) noexcept(
            std::is_nothrow_move_constructible_v<t_type>)
            : value_(std::move(value)) {}

        [[nodiscard]] t_type take_value() && noexcept(std::is_nothrow_move_constructible_v<t_type>) {
            return std::move(value_);
        }

    private:
        t_type value_;
    };

    class suspend_race_ready_after_suspend_type final {
    public:
        suspend_race_ready_after_suspend_type(t_type&& value, std::coroutine_handle<> continuation) noexcept(
            std::is_nothrow_move_constructible_v<t_type>)
            : value_(std::move(value)),
              continuation_(continuation) {}

        [[nodiscard]] std::coroutine_handle<> continuation() const noexcept {
            return continuation_;
        }

        [[nodiscard]] t_type take_value() && noexcept(std::is_nothrow_move_constructible_v<t_type>) {
            return std::move(value_);
        }

    private:
        t_type value_;
        std::coroutine_handle<> continuation_;
    };

    using state_type = std::variant<suspend_race_pending, suspend_race_suspended_type,
        suspend_race_ready_before_suspend_type, suspend_race_ready_after_suspend_type>;

    state_type state_;
};

}  // namespace ruvia::detail

// Building blocks of a worker-bound wait with a single pending waiter, as used
// by channel and one_shot. A composed primitive owns a shared domain state
// with a worker_wait_slot, and each of its cold wait operations owns a
// worker_single_wait_awaiter by value:
//
//   await_ready    start(), then under lock_state() either complete_ready()
//                  with an outcome the domain already has, or
//                  complete_or_publish() to become the pending waiter;
//   await_suspend  suspend();
//   await_resume   take_result().
//
// Domain events complete the pending waiter through the slot under the same
// lock; a worker_shutdown_registration routes worker_stopping there.

namespace ruvia {

template <typename t_type, typename state_type>
    requires std::is_trivially_move_constructible_v<t_type>
class worker_single_wait_awaiter;
template <typename t_type, typename state_type>
    requires std::is_trivially_move_constructible_v<t_type>
class worker_wait_slot;

// A shared owner that links itself into its worker's shutdown registry
// through a worker_shutdown_registration member named
// shutdown_registration_ (see make_worker_wait_state()).
template <typename state_type>
concept worker_shutdown_registered =
    worker_shutdown_observer<state_type> &&
    std::same_as<decltype(state_type::shutdown_registration_), worker_shutdown_registration>;

// The domain state contract: the bound worker, the lock guarding the slot,
// the slot itself, and the shutdown registration that routes the worker's
// stopping notification to the slot. Every slot member is called with that
// lock held.
template <typename state_type, typename t_type>
concept worker_wait_state =
    std::is_trivially_move_constructible_v<t_type> && worker_shutdown_registered<state_type> &&
    requires(state_type& state, const state_type& const_state) {
        { const_state.worker() } -> std::same_as<const worker_ref&>;
        state.lock_state();
        { state.wait_slot() } -> std::same_as<worker_wait_slot<t_type, state_type>&>;
    };

// The single pending-waiter attachment of a domain state. The state owns it by
// value next to its domain fields and guards it with its own lock; every
// member is called with that lock held. Every terminal source completes the
// waiter through this one transition, so detachment, generation invalidation,
// result publication, and wake ordering cannot drift between owners.
template <typename t_type, typename state_type>
    requires std::is_trivially_move_constructible_v<t_type>
class worker_wait_slot final {
public:
    using awaiter_type = worker_single_wait_awaiter<t_type, state_type>;

    worker_wait_slot() noexcept = default;
    worker_wait_slot(const worker_wait_slot&) = delete;
    worker_wait_slot& operator=(const worker_wait_slot&) = delete;

    [[nodiscard]] bool occupied() const noexcept {
        return waiter_ != nullptr;
    }

    // Transfers result_value to the attached waiter, which must exist. A
    // throwing payload move leaves the waiter attached and pending. The wake
    // runs before the caller releases its lock: afterwards an already
    // in-flight timer expiry can resume the receiver and destroy the awaiter,
    // so reading its timer registration later would be a use-after-free.
    void complete(worker_wait_result<t_type>&& result_value) {
        auto* waiter = waiter_;
        if (waiter == nullptr) {
            throw std::logic_error("worker wait slot has no pending waiter");
        }
        const bool wake = waiter->complete_result(std::move(result_value));
        detach_all();
        if (wake) {
            waiter->wake();
        }
    }

    // Completes the attached waiter, if any, with a terminal status.
    void complete(worker_wait_status status) noexcept {
        auto* waiter = std::exchange(waiter_, nullptr);
        generation_ = 0;
        if (waiter != nullptr && waiter->complete_status(status)) {
            waiter->wake();
        }
    }

private:
    friend awaiter_type;

    [[nodiscard]] std::uint64_t reserve_generation() noexcept {
        if (++next_generation_ == 0) {
            ++next_generation_;
        }
        return next_generation_;
    }

    void publish(awaiter_type& waiter, std::uint64_t generation) noexcept {
        waiter_ = &waiter;
        generation_ = generation;
    }

    [[nodiscard]] bool holds(const awaiter_type* waiter) const noexcept {
        return waiter_ == waiter;
    }

    // Detaches waiter without completing it; returns false when it was no
    // longer attached.
    [[nodiscard]] bool detach(const awaiter_type* waiter) noexcept {
        if (waiter_ != waiter) {
            return false;
        }
        detach_all();
        return true;
    }

    // A stale generation belongs to a wait that already completed.
    void cancel(std::uint64_t generation) noexcept {
        if (waiter_ != nullptr && generation_ == generation) {
            complete(worker_wait_status::cancelled);
        }
    }

    void detach_all() noexcept {
        waiter_ = nullptr;
        generation_ = 0;
    }

    awaiter_type* waiter_{nullptr};
    std::uint64_t generation_{0};
    std::uint64_t next_generation_{0};
};

// Shared storage and transition machinery for one worker-bound waiter. Domain
// awaiters own this component by value and publish its typed address directly,
// without an outer-owner pointer, allocation, virtual dispatch, or type erasure.
// state_type must satisfy worker_wait_state<state_type, t_type>.
//
// A relative timeout starts when the wait starts; an absolute deadline that has
// already passed completes the wait as timed_out without suspending. Once the worker's timer
// queue stops, a timed wait completes as worker_stopping.
//
// The awaiter is cold until start(): nothing publishes its address before
// then, so a cold awaiter may move; a started one must stay address-stable.
template <typename t_type, typename state_type>
    requires std::is_trivially_move_constructible_v<t_type>
class worker_single_wait_awaiter final {
public:
    worker_single_wait_awaiter(std::shared_ptr<state_type> state_value, operation_options options) noexcept
        : state_(std::move(state_value)),
          timeout_(options.timeout_),
          stop_token_(std::move(options.stop_token_)) {
        static_assert(worker_wait_state<state_type, t_type>);
    }

    worker_single_wait_awaiter(const worker_single_wait_awaiter&) = delete;
    worker_single_wait_awaiter& operator=(const worker_single_wait_awaiter&) = delete;
    worker_single_wait_awaiter(worker_single_wait_awaiter&& other) noexcept
        : state_(std::move(other.state_)),
          timeout_(other.timeout_),
          deadline_(other.deadline_),
          stop_token_(std::move(other.stop_token_)) {
        if (other.started_) {
            std::terminate();
        }
    }
    worker_single_wait_awaiter& operator=(worker_single_wait_awaiter&&) = delete;

    // The first step of every domain await_ready: a wait starts once, on the
    // worker its state is bound to. A moved-from awaiter has no state.
    void start(const char* affinity_message) {
        if (started_ || !state_) {
            throw std::logic_error("worker wait cannot be awaited");
        }
        if (!state_->worker().is_current()) {
            throw std::logic_error(affinity_message);
        }
        started_ = true;
        if (timeout_.has_value()) {
            deadline_ = timeout_->deadline_from(std::chrono::steady_clock::now());
        }
    }

    [[nodiscard]] state_type& state() const noexcept {
        return *state_;
    }

    // Completes a started wait from await_ready, before it can suspend, with
    // an outcome the domain already holds.
    void complete_ready(worker_wait_result<t_type>&& result_value) {
        (void)complete_result(std::move(result_value));
    }

    void complete_ready(worker_wait_status status) {
        (void)complete_result(worker_wait_result<t_type>::with_status(status));
    }

    // The pending-receiver step shared by every domain awaiter, called with the
    // state's lock held once the domain found nothing to deliver: completes an
    // already stopped or expired wait, otherwise publishes it as the single
    // pending receiver. Returns true when the wait completed without pending.
    [[nodiscard]] bool complete_or_publish(const char* occupied_message) {
        if (stop_token_.stop_requested()) {
            complete_ready(worker_wait_status::cancelled);
            return true;
        }
        if (deadline_.has_value() && *deadline_ <= std::chrono::steady_clock::now()) {
            complete_ready(worker_wait_status::timed_out);
            return true;
        }
        if (state().wait_slot().occupied()) {
            throw std::logic_error(occupied_message);
        }
        publish();
        return false;
    }

    [[nodiscard]] bool suspend(std::coroutine_handle<> continuation) {
        auto& owner_value = state();
        auto* waiter = this;
        bool suspended = false;
        {
            auto lock = owner_value.lock_state();
            auto& slot = owner_value.wait_slot();
            // Arm the deadline before publishing the continuation: the timer only
            // fires on this worker after suspension, and a stopped timer queue can
            // still complete the wait synchronously. A published waiter that is no
            // longer attached has already completed and needs no deadline.
            if (deadline_ && slot.holds(waiter)) {
                auto status = worker_timer_schedule_status::worker_stopping;
                try {
                    status = owner_value.worker().schedule_timer(timer_, *deadline_,
                        [waiter](worker_timer_outcome outcome) noexcept {
                            waiter->on_timer_completed(outcome);
                        });
                } catch (...) {
                    (void)slot.detach(waiter);
                    throw;
                }
                if (status == worker_timer_schedule_status::worker_stopping) {
                    (void)slot.detach(waiter);
                    (void)complete_status(worker_wait_status::worker_stopping);
                }
            }
            // Completion may arrive from another thread. Publish the
            // continuation under the same lock as the result transition.
            suspended = completion_.suspend(continuation);
        }
        // Registered only once the wait really suspends. A stop requested since
        // await_ready runs the callback inline, which completes the wait as
        // cancelled under the state lock before request_stop() returns, so a
        // value sent on the same stack afterwards is queued, not delivered to a
        // cancelled receiver. The callback fits the inline storage of
        // move_only_function, so registration cannot fail.
        if (suspended && generation_ != 0) {
            register_cancellation();
        }
        return suspended;
    }

    [[nodiscard]] worker_wait_result<t_type> take_result() {
        return completion_.take_value();
    }

private:
    friend class worker_wait_slot<t_type, state_type>;

    [[nodiscard]] bool complete_result(worker_wait_result<t_type>&& result_value) {
        return completion_.complete(std::move(result_value));
    }

    [[nodiscard]] bool complete_status(worker_wait_status status) noexcept {
        return completion_.complete(worker_wait_result<t_type>::with_status(status));
    }

    // The cancellation generation exists only for a pending wait.
    void publish() noexcept {
        auto& slot = state().wait_slot();
        generation_ = stop_token_.stoppable() ? slot.reserve_generation() : 0;
        slot.publish(*this, generation_);
    }

    // Completion has already detached the intrusive waiter before wake() runs.
    // A failed continuation dispatch is therefore unrecoverable and shares the
    // same explicit terminate boundary as worker_signal and timer cancellation.
    // A wake on the worker's own thread posts the resume directly.
    void wake() noexcept {
        if (timer_.registered()) {
            timer_.cancel();
        }
        state().worker().defer_resume(completion_.continuation());
    }

    void on_timer_completed(worker_timer_outcome outcome) noexcept {
        std::coroutine_handle<> continuation;
        {
            auto& owner_value = state();
            auto state_lock = owner_value.lock_state();
            // Still pending: a cancellation reaching a pending wait comes only
            // from stop_timers().
            if (!owner_value.wait_slot().detach(this)) {
                return;
            }
            if (!complete_status(outcome == worker_timer_outcome::expired ? worker_wait_status::timed_out
                                                                          : worker_wait_status::worker_stopping)) {
                return;
            }
            continuation = completion_.continuation();
        }
        continuation.resume();
    }

    static void request_cancellation(state_type& state_value, std::uint64_t generation) {
        auto lock = state_value.lock_state();
        state_value.wait_slot().cancel(generation);
    }

    // The callback borrows the awaiter's state_, which outlives the
    // registration (its teardown waits for a running callback), and takes the
    // state lock itself: a stop requested from inside that lock -- a payload
    // move or destructor running under it -- would self-deadlock, exactly as
    // closing the domain from there would. The generation keeps a stale
    // callback from cancelling a later wait on the same slot.
    void register_cancellation() {
        stop_token_.register_callback(stop_registration_,
            [state_ptr = state_.get(), generation = generation_] {
                request_cancellation(*state_ptr, generation);
            });
    }

    std::shared_ptr<state_type> state_;
    std::optional<operation_timeout> timeout_;
    std::optional<std::chrono::steady_clock::time_point> deadline_;
    stop_token stop_token_;
    bool started_{false};
    std::uint64_t generation_{0};
    worker_timer_registration timer_;
    detail::suspend_race_state<worker_wait_result<t_type>> completion_;
    // Last so callback unregistration completes before the state, timer, and
    // borrowed dispatcher begin destruction.
    stop_registration stop_registration_;
};

// Allocates a worker-bound domain state and links its shutdown_registration_
// into the worker's shutdown registry, so the worker's stopping notification
// reaches every state whose endpoints are still alive. The state's constructor
// receives args and must bind the worker that worker() returns. A stopping
// worker throws std::runtime_error.
template <typename state_type, typename... arg_types>
    requires worker_shutdown_registered<state_type> && requires(const state_type& state) {
        { state.worker() } -> std::same_as<const worker_ref&>;
    }
[[nodiscard]] std::shared_ptr<state_type> make_worker_wait_state(arg_types&&... args) {
    auto state_value = std::make_shared<state_type>(std::forward<arg_types>(args)...);
    if (state_value->worker().register_shutdown(state_value, &state_type::shutdown_registration_) !=
        worker_registration_status::registered) {
        throw std::runtime_error("cannot register state on a stopping worker");
    }
    return state_value;
}

}  // namespace ruvia

namespace ruvia {

enum class one_shot_complete_status : std::uint8_t {
    completed,
    already_completed,
    in_progress,
    receiver_closed,
    worker_stopping,
};

template <typename t_type>
class one_shot_completion;

template <typename t_type>
using one_shot_complete_result =
    worker_delivery_result<t_type, one_shot_complete_status, one_shot_complete_status::completed>;

namespace detail {

struct one_shot_pending final {};
struct one_shot_completing final {};
struct one_shot_consumed final {};
struct one_shot_receiver_closed final {};
struct one_shot_worker_stopping final {};

struct one_shot_ready final {};

template <typename t_type>
struct one_shot_state final {
    explicit one_shot_state(worker_ref target)
        : worker_(std::move(target)) {
        if (!worker_.valid()) {
            throw std::invalid_argument("one-shot requires a valid worker");
        }
    }

    void worker_stopping() noexcept;

    [[nodiscard]] std::unique_lock<std::mutex> lock_state() {
        return std::unique_lock(mutex_);
    }
    [[nodiscard]] const worker_ref& worker() const noexcept {
        return worker_;
    }
    [[nodiscard]] worker_wait_slot<std::monostate, one_shot_state>& wait_slot() noexcept {
        return wait_slot_;
    }
    [[nodiscard]] t_type take_ready_value() {
        struct ready_consumption final {
            std::optional<t_type>& value_;
            ~ready_consumption() {
                value_.reset();
            }
        } consumption{ready_value_};
        return std::move(*ready_value_);
    }

    // First member: unlinks from the worker registry after every other member is gone.
    worker_shutdown_registration shutdown_registration_;
    worker_ref worker_;
    std::mutex mutex_;
    using lifecycle_type = std::variant<one_shot_pending, one_shot_completing, one_shot_ready,
        one_shot_consumed, one_shot_receiver_closed, one_shot_worker_stopping>;
    lifecycle_type lifecycle_;
    // After publishing consumed/closed, the thread that claimed ready owns this
    // stable slot until its value has been moved or destroyed outside mutex_.
    std::optional<t_type> ready_value_;
    worker_wait_slot<std::monostate, one_shot_state<t_type>> wait_slot_;
};

template <typename t_type>
void one_shot_state<t_type>::worker_stopping() noexcept {
    std::lock_guard lock(mutex_);
    // Delivered, consumed and closed outcomes stay terminal; only an
    // unresolved exchange is ended by worker shutdown.
    if (std::holds_alternative<one_shot_pending>(lifecycle_) ||
        std::holds_alternative<one_shot_completing>(lifecycle_)) {
        lifecycle_.template emplace<one_shot_worker_stopping>();
    }
    wait_slot_.complete(worker_wait_status::worker_stopping);
}

}  // namespace detail

template <typename t_type>
class one_shot_completion final {
public:
    one_shot_completion() noexcept = default;

    [[nodiscard]] bool valid() const noexcept {
        return state_ != nullptr;
    }

    // Keep this completion handle alive through the call, including payload
    // moves and destruction. Concurrent attempts report in_progress while the
    // first value is being constructed.
    [[nodiscard]] one_shot_complete_result<t_type> complete(t_type value) const {
        // Payload code may synchronously rebind this handle while the call is active.
        const auto state = state_;
        if (state == nullptr) {
            return detail::worker_delivery_access::reject<one_shot_complete_result<t_type>>(
                one_shot_complete_status::receiver_closed, std::move(value));
        }
        std::optional<one_shot_complete_status> rejection;
        {
            std::lock_guard lock(state->mutex_);
            if (std::holds_alternative<detail::one_shot_ready>(state->lifecycle_) ||
                std::holds_alternative<detail::one_shot_consumed>(state->lifecycle_)) {
                rejection = one_shot_complete_status::already_completed;
            } else if (std::holds_alternative<detail::one_shot_completing>(state->lifecycle_)) {
                rejection = one_shot_complete_status::in_progress;
            } else if (std::holds_alternative<detail::one_shot_worker_stopping>(state->lifecycle_)) {
                rejection = one_shot_complete_status::worker_stopping;
            } else if (std::holds_alternative<detail::one_shot_receiver_closed>(state->lifecycle_)) {
                rejection = one_shot_complete_status::receiver_closed;
            } else if (!state->worker_.accepting()) {
                rejection = one_shot_complete_status::worker_stopping;
            } else {
                assert(std::holds_alternative<detail::one_shot_pending>(state->lifecycle_));
                state->lifecycle_.template emplace<detail::one_shot_completing>();
            }
        }
        if (rejection) {
            return detail::worker_delivery_access::reject<one_shot_complete_result<t_type>>(*rejection, std::move(value));
        }
        try {
            state->ready_value_.emplace(std::move(value));
        } catch (...) {
            std::lock_guard lock(state->mutex_);
            if (std::holds_alternative<detail::one_shot_completing>(state->lifecycle_)) {
                state->lifecycle_.template emplace<detail::one_shot_pending>();
            }
            throw;
        }
        {
            std::lock_guard lock(state->mutex_);
            if (std::holds_alternative<detail::one_shot_receiver_closed>(state->lifecycle_)) {
                rejection = one_shot_complete_status::receiver_closed;
            } else if (std::holds_alternative<detail::one_shot_worker_stopping>(state->lifecycle_) ||
                       !state->worker_.accepting()) {
                state->lifecycle_.template emplace<detail::one_shot_worker_stopping>();
                state->wait_slot_.complete(worker_wait_status::worker_stopping);
                rejection = one_shot_complete_status::worker_stopping;
            } else if (state->wait_slot_.occupied()) {
                state->lifecycle_.template emplace<detail::one_shot_consumed>();
                state->wait_slot_.complete(worker_wait_result<std::monostate>::with_value(std::monostate{}));
            } else {
                state->lifecycle_.template emplace<detail::one_shot_ready>();
            }
        }
        if (rejection) {
            return detail::worker_delivery_access::reject<one_shot_complete_result<t_type>>(*rejection, state->take_ready_value());
        }
        return detail::worker_delivery_access::accept<one_shot_complete_result<t_type>>();
    }

private:
    explicit one_shot_completion(std::shared_ptr<detail::one_shot_state<t_type>> state_value)
        : state_(std::move(state_value)) {}
    std::shared_ptr<detail::one_shot_state<t_type>> state_;
    template <typename u_type>
    friend auto make_one_shot(worker_ref);
};

template <typename t_type>
class one_shot_receiver final {
public:
    one_shot_receiver() = delete;
    one_shot_receiver(const one_shot_receiver&) = delete;
    one_shot_receiver& operator=(const one_shot_receiver&) = delete;
    one_shot_receiver(one_shot_receiver&&) noexcept = default;
    one_shot_receiver& operator=(one_shot_receiver&&) = delete;
    ~one_shot_receiver() {
        close();
    }

    [[nodiscard]] bool valid() const noexcept {
        return state_ != nullptr;
    }
    // The wait operation: single-use, movable only before it is awaited, and
    // started on the receiver's worker by co_await. Dropping a cold one has no
    // effect on the exchange.
    class wait_operation final {
    public:
        wait_operation(wait_operation&&) noexcept = default;
        wait_operation(const wait_operation&) = delete;
        wait_operation& operator=(const wait_operation&) = delete;
        wait_operation& operator=(wait_operation&&) = delete;
        void operator&() const = delete;

        [[nodiscard]] bool await_ready() {
            wait_.start("one-shot wait must run on its bound worker");
            auto& owner_value = wait_.state();
            auto lock = owner_value.lock_state();
            if (std::holds_alternative<detail::one_shot_ready>(owner_value.lifecycle_)) {
                owner_value.lifecycle_.template emplace<detail::one_shot_consumed>();
                wait_.complete_ready(worker_wait_result<std::monostate>::with_value(std::monostate{}));
                return true;
            }
            if (std::holds_alternative<detail::one_shot_worker_stopping>(owner_value.lifecycle_)) {
                wait_.complete_ready(worker_wait_status::worker_stopping);
                return true;
            }
            if (std::holds_alternative<detail::one_shot_receiver_closed>(owner_value.lifecycle_) ||
                std::holds_alternative<detail::one_shot_consumed>(owner_value.lifecycle_)) {
                wait_.complete_ready(worker_wait_status::closed);
                return true;
            }
            assert(std::holds_alternative<detail::one_shot_pending>(owner_value.lifecycle_) ||
                   std::holds_alternative<detail::one_shot_completing>(owner_value.lifecycle_));
            return wait_.complete_or_publish("one-shot supports one pending receiver");
        }

        bool await_suspend(std::coroutine_handle<> continuation) {
            return wait_.suspend(continuation);
        }

        [[nodiscard]] worker_wait_result<t_type> await_resume() {
            auto result = wait_.take_result();
            if (!result.has_value()) {
                return worker_wait_result<t_type>::with_status(result.status());
            }
            return worker_wait_result<t_type>::with_value(wait_.state().take_ready_value());
        }

    private:
        friend class one_shot_receiver;
        using state_type = detail::one_shot_state<t_type>;

        wait_operation(std::shared_ptr<state_type> state_value, operation_options options) noexcept
            : wait_(std::move(state_value), std::move(options)) {}

        worker_single_wait_awaiter<std::monostate, state_type> wait_;
    };

    // The receiver owns the receive side: destroying it closes every cold or
    // running wait. Require a named owner so storing an operation cannot
    // silently turn a temporary receiver into an already-closed operation.
    // A relative options.timeout_ bounds the wait from its start; a deadline
    // that has passed reports timed_out unless a value is ready.
    [[nodiscard]] wait_operation wait(operation_options options = {}) const& {
        return wait_operation(state_, std::move(options));
    }
    wait_operation wait(operation_options = {}) const&& = delete;

    // The worker every wait must run on. Makes the receive-side affinity
    // contract queryable instead of only failing at await time. A moved-from
    // receiver has no bound worker and cannot answer this query.
    [[nodiscard]] const worker_ref& worker() const& noexcept {
        if (!state_) {
            std::terminate();
        }
        return state_->worker_;
    }
    const worker_ref& worker() const&& = delete;

    // Closing discards an unconsumed value before returning and closes every
    // future wait. Its destruction may reenter this receiver or its completion.
    void close() const {
        if (!state_) {
            return;
        }
        const auto state = state_;
        bool discard = false;
        {
            std::lock_guard lock(state->mutex_);
            discard = std::holds_alternative<detail::one_shot_ready>(state->lifecycle_);
            if (!discard && !std::holds_alternative<detail::one_shot_pending>(state->lifecycle_) &&
                !std::holds_alternative<detail::one_shot_completing>(state->lifecycle_)) {
                return;
            }
            state->lifecycle_.template emplace<detail::one_shot_receiver_closed>();
            state->wait_slot_.complete(worker_wait_status::closed);
        }
        if (discard) {
            state->ready_value_.reset();
        }
    }

private:
    explicit one_shot_receiver(std::shared_ptr<detail::one_shot_state<t_type>> state_value)
        : state_(std::move(state_value)) {}
    std::shared_ptr<detail::one_shot_state<t_type>> state_;
    template <typename u_type>
    friend auto make_one_shot(worker_ref);
};

template <typename t_type>
// The value parameter accepts both lvalue and rvalue worker refs before
// transferring the stable dispatcher endpoint into the one-shot state.
// NOLINTNEXTLINE(performance-unnecessary-value-param)
[[nodiscard]] auto make_one_shot(worker_ref worker_value) {
    auto state_value = make_worker_wait_state<detail::one_shot_state<t_type>>(std::move(worker_value));
    one_shot_completion<t_type> completion(state_value);
    return std::pair(std::move(completion), one_shot_receiver<t_type>(std::move(state_value)));
}

}  // namespace ruvia

namespace ruvia {
class pool_lease_acquire;

// Single-worker owner of pool slot leases and their asynchronous wait queue.
// The worker endpoint is retained once by the scheduler. Acquires copy
// their inputs before return and borrow only their scheduler, which must
// outlive them.
class pool_lease_scheduler final {
public:
    pool_lease_scheduler(std::size_t pool_size, const worker_ref& worker_value);

    pool_lease_scheduler(const pool_lease_scheduler&) = delete;
    pool_lease_scheduler& operator=(const pool_lease_scheduler&) = delete;
    pool_lease_scheduler(pool_lease_scheduler&&) = delete;
    pool_lease_scheduler& operator=(pool_lease_scheduler&&) = delete;
    ~pool_lease_scheduler();

    // The returned awaitable is cold: nothing is checked or queued until it is
    // awaited, which must happen on the scheduler's worker. A free slot is
    // leased synchronously without suspending; only a saturated pool waits.
    // The result carries the leased slot index as its value, or closed,
    // worker_stopping, timed_out or cancelled. A timed-out waiter leaves other
    // queued acquisitions unchanged. A relative options.timeout_ bounds the
    // wait from await; a deadline that has passed times out a saturated
    // acquire at once. Once the worker's shutdown notification arrives or its
    // timer queue stops, queued acquires and saturated new ones complete with
    // worker_stopping, as channel and one_shot waits do; a free slot is still
    // leased. It never throws for that.
    [[nodiscard]] pool_lease_acquire acquire(operation_options options = {}) &;
    pool_lease_acquire acquire(operation_options = {}) && = delete;

    // Return a caller-owned slot on the bound worker. A queued acquire takes
    // ownership before this call returns, but its coroutine resumes in a later
    // worker turn, never inline here. Later cancellation or close() cannot
    // replace that committed acquisition.
    void release(std::size_t slot) noexcept;
    // Refuses new acquires and completes every queued acquire with closed;
    // returns false when already closing. Like release(), the queued coroutines
    // resume in a later worker turn, never inline here, so the caller finishes
    // its own teardown before any waiter observes the closure.
    [[nodiscard]] bool close() noexcept;
    [[nodiscard]] bool closing() const noexcept;

private:
    friend class pool_lease_acquire;

    class impl_type;
    std::unique_ptr<impl_type> impl_;
};

// One acquire: single-use, movable only before it is awaited. The scheduler
// must outlive it; destroying a scheduler with live acquires terminates.
class pool_lease_acquire final {
public:
    pool_lease_acquire(pool_lease_acquire&& other) noexcept;
    pool_lease_acquire(const pool_lease_acquire&) = delete;
    pool_lease_acquire& operator=(const pool_lease_acquire&) = delete;
    pool_lease_acquire& operator=(pool_lease_acquire&&) = delete;
    void operator&() const = delete;
    ~pool_lease_acquire();

    [[nodiscard]] bool await_ready();
    [[nodiscard]] bool await_suspend(std::coroutine_handle<> continuation);
    [[nodiscard]] worker_wait_result<std::size_t> await_resume();

private:
    friend class pool_lease_scheduler;

    pool_lease_acquire(pool_lease_scheduler::impl_type& owner_value, std::optional<operation_timeout> timeout,
        stop_token stop_token_value) noexcept;
    void cancel_registrations() noexcept;
    void complete(worker_wait_result<std::size_t> result_value) noexcept;
    [[nodiscard]] std::coroutine_handle<> resolve(worker_wait_result<std::size_t> result_value) noexcept;
    [[nodiscard]] std::coroutine_handle<> resolve(worker_wait_status status) noexcept;

    pool_lease_scheduler::impl_type* owner_;
    std::optional<operation_timeout> timeout_;
    std::optional<std::chrono::steady_clock::time_point> deadline_;
    stop_token stop_token_;
    std::optional<worker_wait_result<std::size_t>> result_;
    std::uint64_t waiter_id_{0};
    std::coroutine_handle<> continuation_{};
    pool_lease_acquire* previous_{nullptr};
    pool_lease_acquire* next_{nullptr};
    worker_timer_registration deadline_timer_;
    stop_registration stop_registration_;
    bool queued_{false};
    bool awaited_{false};
};

}  // namespace ruvia

namespace ruvia {

enum class channel_send_status : std::uint8_t {
    sent,
    // send() only: every slot is in use.
    full,
    closed,
    worker_stopping,
    // send_wait() only: its operation_options ended the wait for a slot.
    timed_out,
    cancelled,
};

template <typename t_type>
class channel_sender;
template <typename t_type>
class channel_send_operation;

struct channel_options final {
    // Bounds buffered and reserved sends. One extra slot permits a value
    // already assigned to a receiver while the buffer fills again.
    // A send may report full while every slot is temporarily in use.
    std::size_t capacity_{0};
};

template <typename t_type>
using channel_send_result =
    worker_delivery_result<t_type, channel_send_status, channel_send_status::sent>;

namespace detail {

struct channel_open final {};
struct channel_closed final {};
struct channel_worker_stopping final {};

template <typename t_type>
struct channel_state final {
    struct node final {
        std::optional<t_type> value_;
        node* next_{nullptr};
    };

    static std::size_t slot_count(std::size_t capacity) {
        if (capacity == 0) {
            throw std::invalid_argument("channel capacity must be nonzero");
        }
        if (capacity == std::numeric_limits<std::size_t>::max()) {
            throw std::length_error("channel capacity is too large");
        }
        return capacity + 1;
    }

    channel_state(worker_ref target, std::size_t requested_capacity)
        : worker_(std::move(target)),
          slots_(slot_count(requested_capacity)),
          capacity_(requested_capacity) {
        if (!worker_.valid()) {
            throw std::invalid_argument("channel requires a valid worker");
        }
        for (auto& slot : slots_) {
            slot.next_ = free_;
            free_ = &slot;
        }
    }

    [[nodiscard]] std::unique_lock<std::mutex> lock_state() {
        return std::unique_lock(mutex_);
    }

    [[nodiscard]] const worker_ref& worker() const noexcept {
        return worker_;
    }

    [[nodiscard]] worker_wait_slot<node*, channel_state>& wait_slot() noexcept {
        return wait_slot_;
    }

    [[nodiscard]] std::optional<channel_send_status> check_open_locked() const noexcept {
        if (std::holds_alternative<channel_closed>(lifecycle_)) {
            return channel_send_status::closed;
        }
        if (std::holds_alternative<channel_worker_stopping>(lifecycle_) || !worker_.accepting()) {
            return channel_send_status::worker_stopping;
        }
        return std::nullopt;
    }

    // Called under mutex_. Reservations hold capacity until publication.
    [[nodiscard]] node* reserve_node() noexcept {
        if (queued_ + reserved_ == capacity_ || free_ == nullptr) {
            return nullptr;
        }
        auto* slot = free_;
        free_ = slot->next_;
        slot->next_ = nullptr;
        ++reserved_;
        return slot;
    }

    void enqueue(node* slot) noexcept {
        slot->next_ = nullptr;
        if (tail_ != nullptr) {
            tail_->next_ = slot;
        } else {
            head_ = slot;
        }
        tail_ = slot;
        ++queued_;
    }

    [[nodiscard]] node* dequeue() noexcept {
        auto* slot = head_;
        if (slot == nullptr) {
            return nullptr;
        }
        head_ = slot->next_;
        if (head_ == nullptr) {
            tail_ = nullptr;
        }
        slot->next_ = nullptr;
        --queued_;
        return slot;
    }

    void recycle(node* slot) noexcept {
        slot->next_ = free_;
        free_ = slot;
    }

    // Called under mutex_ whenever capacity may have been released: hands
    // each free slot, in FIFO order, to a waiting send_wait() as a
    // reservation it fills once it resumes on its own worker.
    void grant_send_waiters() noexcept;
    // Called under mutex_ by close and worker shutdown.
    void complete_send_waiters(channel_send_status status) noexcept;
    void enqueue_send_waiter(channel_send_operation<t_type>& waiter) noexcept;
    // Returns false when the waiter is no longer queued.
    [[nodiscard]] bool remove_send_waiter(channel_send_operation<t_type>& waiter) noexcept;

    // Called under mutex_: publishes a filled reservation to the pending
    // receiver or the queue. Returns the rejection when the channel closed or
    // stopped since the reservation, leaving the value in the slot.
    [[nodiscard]] std::optional<channel_send_status> publish_reserved(node* slot) {
        --reserved_;
        auto rejection = check_open_locked();
        if (!rejection) {
            if (wait_slot_.occupied()) {
                wait_slot_.complete(worker_wait_result<node*>::with_value(slot));
                // A direct handoff never occupied the queue.
                grant_send_waiters();
            } else {
                enqueue(slot);
            }
        }
        return rejection;
    }

    // Called under mutex_ when a filled reservation is abandoned.
    void release_reserved(node* slot) noexcept {
        --reserved_;
        recycle(slot);
        grant_send_waiters();
    }

    // Owns one reserved or claimed slot; user destruction runs without mutex_.
    class node_guard final {
    public:
        node_guard(channel_state& state, node* slot) noexcept
            : state_(state),
              slot_(slot) {}
        node_guard(const node_guard&) = delete;
        node_guard& operator=(const node_guard&) = delete;
        ~node_guard() {
            if (slot_ != nullptr) {
                slot_->value_.reset();
                auto lock = state_.lock_state();
                state_.recycle(slot_);
                state_.grant_send_waiters();
            }
        }
        void release() noexcept {
            slot_ = nullptr;
        }

    private:
        channel_state& state_;
        node* slot_;
    };

    // First member: unlinks from the worker registry after every other member is gone.
    worker_shutdown_registration shutdown_registration_;
    worker_ref worker_;
    std::mutex mutex_;
    std::vector<node> slots_;
    const std::size_t capacity_;
    node* free_{nullptr};
    node* head_{nullptr};
    node* tail_{nullptr};
    std::size_t queued_{0};
    std::size_t reserved_{0};
    using lifecycle_type = std::variant<channel_open, channel_closed, channel_worker_stopping>;
    lifecycle_type lifecycle_;
    bool receiver_closed_{false};
    worker_wait_slot<node*, channel_state<t_type>> wait_slot_;
    channel_send_operation<t_type>* send_head_{nullptr};
    channel_send_operation<t_type>* send_tail_{nullptr};

    void close(bool retire_receiver);
    void worker_stopping() noexcept;

private:
    void discard_buffered();
};

template <typename t_type>
void channel_state<t_type>::close(bool retire_receiver) {
    {
        auto lock = lock_state();
        receiver_closed_ = receiver_closed_ || retire_receiver;
        if (std::holds_alternative<channel_open>(lifecycle_)) {
            lifecycle_.template emplace<channel_closed>();
            wait_slot_.complete(worker_wait_status::closed);
            complete_send_waiters(channel_send_status::closed);
        }
    }
    if (retire_receiver) {
        discard_buffered();
    }
}

// Retirement leaves the lifecycle closed or stopping, so no sender can publish
// again and the queue only shrinks here. Buffered values die now, on the
// retiring thread, rather than with the last sender wherever it drops. Each
// value is unpublished under the lock but destroyed in its slot after the lock
// is released, so its destructor may close this channel or drop its endpoints;
// the caller keeps the state alive until the drain returns.
template <typename t_type>
void channel_state<t_type>::discard_buffered() {
    for (;;) {
        node* retired;
        {
            auto lock = lock_state();
            retired = dequeue();
        }
        if (retired == nullptr) {
            return;
        }
        node_guard guard(*this, retired);
    }
}

template <typename t_type>
void channel_state<t_type>::worker_stopping() noexcept {
    auto lock = lock_state();
    lifecycle_.template emplace<channel_worker_stopping>();
    wait_slot_.complete(worker_wait_status::worker_stopping);
    complete_send_waiters(channel_send_status::worker_stopping);
}

}  // namespace detail

// One send_wait(): single-use, movable only before it is awaited. It owns the
// value until delivery and returns it in the result otherwise. A slot granted
// while it waits is reserved for it and filled when it resumes on the worker
// that awaited it.
template <typename t_type>
class channel_send_operation final {
public:
    channel_send_operation(const channel_send_operation&) = delete;
    channel_send_operation& operator=(const channel_send_operation&) = delete;
    channel_send_operation(channel_send_operation&& other) noexcept(std::is_nothrow_move_constructible_v<t_type>)
        : state_(std::move(other.state_)),
          value_(std::move(other.value_)),
          timeout_(other.timeout_),
          stop_token_(std::move(other.stop_token_)) {
        if (other.started_) {
            std::terminate();
        }
    }
    channel_send_operation& operator=(channel_send_operation&&) = delete;
    void operator&() const = delete;

    ~channel_send_operation() {
        // A frame destroyed while suspended must not leave a dangling waiter.
        if (queued_) {
            auto lock = state_->lock_state();
            (void)state_->remove_send_waiter(*this);
        }
    }

    [[nodiscard]] bool await_ready() {
        if (started_ || !value_.has_value()) {
            throw std::logic_error("channel send cannot be awaited");
        }
        if (!state_) {
            started_ = true;
            complete_ready({channel_send_status::closed, nullptr});
            return true;
        }
        auto worker_value = detail::worker_ref_access::current();
        if (!worker_value.valid()) {
            throw std::logic_error("channel send_wait must be awaited on a worker");
        }
        started_ = true;
        worker_ = std::move(worker_value);
        const auto now = std::chrono::steady_clock::now();
        if (timeout_.has_value()) {
            deadline_ = timeout_->deadline_from(now);
        }
        auto& state = *state_;
        auto lock = state.lock_state();
        if (const auto rejection = state.check_open_locked()) {
            complete_ready({*rejection, nullptr});
            return true;
        }
        if (auto* slot = state.reserve_node()) {
            complete_ready({channel_send_status::sent, slot});
            return true;
        }
        if (stop_token_.stop_requested()) {
            complete_ready({channel_send_status::cancelled, nullptr});
            return true;
        }
        if (deadline_.has_value() && *deadline_ <= now) {
            complete_ready({channel_send_status::timed_out, nullptr});
            return true;
        }
        state.enqueue_send_waiter(*this);
        return false;
    }

    [[nodiscard]] bool await_suspend(std::coroutine_handle<> continuation) {
        auto& state = *state_;
        bool suspended = false;
        {
            auto lock = state.lock_state();
            // A timer is always armed on the awaiting worker, at the deadline or
            // never, so that worker's timer shutdown also ends the wait.
            if (queued_) {
                auto status = worker_timer_schedule_status::worker_stopping;
                try {
                    status = worker_.schedule_timer(timer_,
                        deadline_.value_or(std::chrono::steady_clock::time_point::max()),
                        [this](worker_timer_outcome outcome) noexcept { on_timer_completed(outcome); });
                } catch (...) {
                    (void)state.remove_send_waiter(*this);
                    throw;
                }
                if (status == worker_timer_schedule_status::worker_stopping) {
                    (void)state.remove_send_waiter(*this);
                    (void)completion_.complete({channel_send_status::worker_stopping, nullptr});
                }
            }
            suspended = completion_.suspend(continuation);
        }
        // Registered only once the wait really suspends; a stop already
        // requested runs the callback inline and completes the wait.
        if (suspended && stop_token_.stoppable()) {
            stop_token_.register_callback(stop_registration_, [this] { request_cancellation(); });
        }
        return suspended;
    }

    [[nodiscard]] channel_send_result<t_type> await_resume() {
        using result_type = channel_send_result<t_type>;
        const auto outcome = completion_.take_value();
        if (outcome.status_ != channel_send_status::sent) {
            return detail::worker_delivery_access::reject<result_type>(outcome.status_, std::move(*value_));
        }
        auto& state = *state_;
        typename state_type::node_guard guard(state, outcome.slot_);
        try {
            outcome.slot_->value_.emplace(std::move(*value_));
        } catch (...) {
            auto lock = state.lock_state();
            state.release_reserved(outcome.slot_);
            guard.release();
            throw;
        }
        std::optional<channel_send_status> rejection;
        {
            auto lock = state.lock_state();
            rejection = state.publish_reserved(outcome.slot_);
            if (!rejection) {
                guard.release();
            }
        }
        if (rejection) {
            return detail::worker_delivery_access::reject<result_type>(*rejection, std::move(*outcome.slot_->value_));
        }
        return detail::worker_delivery_access::accept<result_type>();
    }

private:
    using state_type = detail::channel_state<t_type>;
    using node = typename state_type::node;

    friend class channel_sender<t_type>;
    friend struct detail::channel_state<t_type>;

    struct outcome_type final {
        channel_send_status status_;
        // The reserved slot when status_ is sent.
        node* slot_;
    };

    channel_send_operation(std::shared_ptr<state_type> state_value, t_type&& value, operation_options options) noexcept(
        std::is_nothrow_move_constructible_v<t_type>)
        : state_(std::move(state_value)),
          value_(std::move(value)),
          timeout_(options.timeout_),
          stop_token_(std::move(options.stop_token_)) {}

    void complete_ready(outcome_type outcome) noexcept {
        (void)completion_.complete(std::move(outcome));
    }

    // Under the state lock, once removed from the queue. The wake runs before
    // the lock is released, so a racing timer expiry cannot resume and destroy
    // this operation first.
    void complete_locked(outcome_type outcome) noexcept {
        if (completion_.complete(std::move(outcome))) {
            if (timer_.registered()) {
                timer_.cancel();
            }
            worker_.defer_resume(completion_.continuation());
        }
    }

    void on_timer_completed(worker_timer_outcome outcome) noexcept {
        std::coroutine_handle<> continuation;
        {
            auto lock = state_->lock_state();
            if (!state_->remove_send_waiter(*this)) {
                return;
            }
            if (!completion_.complete({outcome == worker_timer_outcome::expired ? channel_send_status::timed_out
                                                                                : channel_send_status::worker_stopping,
                    nullptr})) {
                return;
            }
            continuation = completion_.continuation();
        }
        continuation.resume();
    }

    // Unregistration waits for a running callback, so this operation and its
    // state outlive it.
    void request_cancellation() noexcept {
        auto lock = state_->lock_state();
        if (state_->remove_send_waiter(*this)) {
            complete_locked({channel_send_status::cancelled, nullptr});
        }
    }

    std::shared_ptr<state_type> state_;
    std::optional<t_type> value_;
    std::optional<operation_timeout> timeout_;
    stop_token stop_token_;
    worker_ref worker_;
    std::optional<std::chrono::steady_clock::time_point> deadline_;
    // Queue links, guarded by the state lock.
    channel_send_operation* previous_{nullptr};
    channel_send_operation* next_{nullptr};
    bool queued_{false};
    bool started_{false};
    worker_timer_registration timer_;
    detail::suspend_race_state<outcome_type> completion_;
    // Last so callback unregistration completes before anything it touches
    // begins destruction.
    stop_registration stop_registration_;
};

namespace detail {

template <typename t_type>
void channel_state<t_type>::enqueue_send_waiter(channel_send_operation<t_type>& waiter) noexcept {
    waiter.previous_ = send_tail_;
    waiter.next_ = nullptr;
    waiter.queued_ = true;
    if (send_tail_ != nullptr) {
        send_tail_->next_ = std::addressof(waiter);
    } else {
        send_head_ = std::addressof(waiter);
    }
    send_tail_ = std::addressof(waiter);
}

template <typename t_type>
bool channel_state<t_type>::remove_send_waiter(channel_send_operation<t_type>& waiter) noexcept {
    if (!waiter.queued_) {
        return false;
    }
    if (waiter.previous_ != nullptr) {
        waiter.previous_->next_ = waiter.next_;
    } else {
        send_head_ = waiter.next_;
    }
    if (waiter.next_ != nullptr) {
        waiter.next_->previous_ = waiter.previous_;
    } else {
        send_tail_ = waiter.previous_;
    }
    waiter.previous_ = nullptr;
    waiter.next_ = nullptr;
    waiter.queued_ = false;
    return true;
}

template <typename t_type>
void channel_state<t_type>::grant_send_waiters() noexcept {
    while (send_head_ != nullptr && std::holds_alternative<channel_open>(lifecycle_)) {
        auto* slot = reserve_node();
        if (slot == nullptr) {
            return;
        }
        auto& waiter = *send_head_;
        (void)remove_send_waiter(waiter);
        waiter.complete_locked({channel_send_status::sent, slot});
    }
}

template <typename t_type>
void channel_state<t_type>::complete_send_waiters(channel_send_status status) noexcept {
    while (send_head_ != nullptr) {
        auto& waiter = *send_head_;
        (void)remove_send_waiter(waiter);
        waiter.complete_locked({status, nullptr});
    }
}

}  // namespace detail

template <typename t_type>
class channel_sender final {
public:
    channel_sender() noexcept = default;

    [[nodiscard]] bool valid() const noexcept {
        return state_ != nullptr;
    }

    // The sender must remain alive throughout the call, including payload
    // moves and destruction. Concurrent sends are delivered in publication
    // order; closure during a payload move returns that value as rejected.
    // Never reports timed_out or cancelled.
    [[nodiscard]] channel_send_result<t_type> send(t_type value) const {
        // Payload code may synchronously rebind this handle while the call is active.
        const auto state = state_;
        if (state == nullptr) {
            return detail::worker_delivery_access::reject<channel_send_result<t_type>>(channel_send_status::closed, std::move(value));
        }
        using state_type = detail::channel_state<t_type>;
        typename state_type::node* slot = nullptr;
        std::optional<channel_send_status> rejection;
        {
            auto lock = state->lock_state();
            rejection = state->check_open_locked();
            if (!rejection && (slot = state->reserve_node()) == nullptr) {
                rejection = channel_send_status::full;
            }
        }
        if (rejection) {
            return detail::worker_delivery_access::reject<channel_send_result<t_type>>(*rejection, std::move(value));
        }

        typename state_type::node_guard guard(*state, slot);
        try {
            slot->value_.emplace(std::move(value));
        } catch (...) {
            auto lock = state->lock_state();
            state->release_reserved(slot);
            guard.release();
            throw;
        }
        {
            auto lock = state->lock_state();
            rejection = state->publish_reserved(slot);
            if (!rejection) {
                guard.release();
            }
        }
        if (rejection) {
            return detail::worker_delivery_access::reject<channel_send_result<t_type>>(*rejection, std::move(*slot->value_));
        }
        return detail::worker_delivery_access::accept<channel_send_result<t_type>>();
    }

    // The waiting form of send(): a cold operation that, once awaited, takes a
    // free slot at once or suspends until the receiver releases one. Waiting
    // senders are served in FIFO order. It must be awaited on a worker thread,
    // which is where it resumes (std::logic_error elsewhere); the channel's
    // worker need not be that worker. A value that is not delivered -- the
    // channel closed or its worker stopped, the awaiting worker stopped
    // (worker_stopping), options.timeout_ elapsed (timed_out), or
    // options.stop_token_ fired (cancelled) -- returns in the result. Without
    // a timeout or stop token, the wait lasts until a slot frees, the channel
    // closes, or either worker stops.
    [[nodiscard]] channel_send_operation<t_type> send_wait(t_type value, operation_options options = {}) const& {
        return channel_send_operation<t_type>(state_, std::move(value), std::move(options));
    }
    channel_send_operation<t_type> send_wait(t_type, operation_options = {}) const&& = delete;

    // Stop sending while allowing the receiver to drain buffered values.
    void close() const {
        if (state_) {
            state_->close(false);
        }
    }

private:
    explicit channel_sender(std::shared_ptr<detail::channel_state<t_type>> state_value)
        : state_(std::move(state_value)) {}

    std::shared_ptr<detail::channel_state<t_type>> state_;
    template <typename u_type>
    friend auto make_channel(worker_ref, channel_options);
};

template <typename t_type>
class channel_receiver final {
public:
    channel_receiver() = delete;
    channel_receiver(const channel_receiver&) = delete;
    channel_receiver& operator=(const channel_receiver&) = delete;
    channel_receiver(channel_receiver&&) noexcept = default;
    channel_receiver& operator=(channel_receiver&&) = delete;

    ~channel_receiver() {
        close();
    }

    [[nodiscard]] bool valid() const noexcept {
        return state_ != nullptr;
    }
    // The receive operation: single-use, movable only before it is awaited,
    // and started on the receiver's worker by co_await. Dropping a cold one
    // has no effect on the channel.
    class receive_operation final {
    public:
        receive_operation(receive_operation&&) noexcept = default;
        receive_operation(const receive_operation&) = delete;
        receive_operation& operator=(const receive_operation&) = delete;
        receive_operation& operator=(receive_operation&&) = delete;
        void operator&() const = delete;

        [[nodiscard]] bool await_ready() {
            wait_.start("channel receive must run on its bound worker");
            auto& owner_value = wait_.state();
            auto lock = owner_value.lock_state();
            if (!owner_value.receiver_closed_) {
                if (auto* item = owner_value.dequeue()) {
                    owner_value.grant_send_waiters();
                    wait_.complete_ready(worker_wait_result<node*>::with_value(item));
                    return true;
                }
            }
            if (std::holds_alternative<detail::channel_worker_stopping>(owner_value.lifecycle_)) {
                wait_.complete_ready(worker_wait_status::worker_stopping);
                return true;
            }
            if (std::holds_alternative<detail::channel_closed>(owner_value.lifecycle_)) {
                wait_.complete_ready(worker_wait_status::closed);
                return true;
            }
            assert(std::holds_alternative<detail::channel_open>(owner_value.lifecycle_));
            return wait_.complete_or_publish("channel supports one pending receiver");
        }

        bool await_suspend(std::coroutine_handle<> continuation) {
            return wait_.suspend(continuation);
        }

        [[nodiscard]] worker_wait_result<t_type> await_resume() {
            auto result = wait_.take_result();
            if (!result.has_value()) {
                return worker_wait_result<t_type>::with_status(result.status());
            }
            auto* item = result.value();
            typename state_type::node_guard guard(wait_.state(), item);
            return worker_wait_result<t_type>::with_value(std::move(*item->value_));
        }

    private:
        friend class channel_receiver;
        using state_type = detail::channel_state<t_type>;
        using node = typename state_type::node;

        receive_operation(std::shared_ptr<state_type> state_value, operation_options options) noexcept
            : wait_(std::move(state_value), std::move(options)) {}

        worker_single_wait_awaiter<node*, state_type> wait_;
    };

    // The receiver owns the receive side: destroying it prevents cold receives
    // from consuming buffered values and closes pending receives. Require a
    // named owner so storing an operation cannot silently turn a temporary
    // receiver into an already-closed operation. A relative options.timeout_
    // bounds the wait from its start; a deadline that has passed reports
    // timed_out once nothing is buffered.
    [[nodiscard]] receive_operation receive(operation_options options = {}) const& {
        return receive_operation(state_, std::move(options));
    }
    receive_operation receive(operation_options = {}) const&& = delete;

    // The worker every receive must run on. Makes the receive-side affinity
    // contract queryable instead of only failing at await time. A moved-from
    // receiver has no bound worker and cannot answer this query.
    [[nodiscard]] const worker_ref& worker() const& noexcept {
        if (!state_) {
            std::terminate();
        }
        return state_->worker_;
    }
    const worker_ref& worker() const&& = delete;

    // Retire the receive side: buffered values are destroyed on the calling
    // thread and later receives report closed. Worker shutdown keeps its
    // worker_stopping outcome. The shared state itself lives until all
    // endpoints and tasks owning it retire.
    void close() const {
        if (state_) {
            // A buffered payload destructor may destroy this receiver and the
            // last sender, so the drain runs on its own owner of the state.
            const auto pinned = state_;
            pinned->close(true);
        }
    }

private:
    explicit channel_receiver(std::shared_ptr<detail::channel_state<t_type>> state_value)
        : state_(std::move(state_value)) {}

    std::shared_ptr<detail::channel_state<t_type>> state_;
    template <typename u_type>
    friend auto make_channel(worker_ref, channel_options);
};

template <typename t_type>
// The value parameter accepts both lvalue and rvalue worker refs before
// transferring the stable dispatcher endpoint into the channel state.
// NOLINTNEXTLINE(performance-unnecessary-value-param)
[[nodiscard]] auto make_channel(worker_ref worker_value, channel_options options) {
    auto state_value = make_worker_wait_state<detail::channel_state<t_type>>(
        std::move(worker_value), options.capacity_);
    channel_sender<t_type> sender(state_value);
    return std::pair(std::move(sender), channel_receiver<t_type>(std::move(state_value)));
}

}  // namespace ruvia
