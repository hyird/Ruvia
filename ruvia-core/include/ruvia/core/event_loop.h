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
#include <ranges>
#include <ratio>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <asio/associated_allocator.hpp>
#include <asio/associated_executor.hpp>
#include <asio/async_result.hpp>
#include <asio/awaitable.hpp>
#include <asio/bind_allocator.hpp>
#include <asio/disposition.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/address.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/post.hpp>
#include <asio/use_awaitable.hpp>

#include "ruvia/core/task.h"

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

enum class worker_runtime_state : std::uint8_t {
    ready,
    running,
    stopping,
    stopped,
};

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

class task_scope final {
public:
    // The worker is borrowed: the scope must not outlive the caller's
    // address-stable worker_ref, matching the borrow-only hot-path rule.
    explicit task_scope(const worker_ref& worker_value);
    task_scope(worker_ref&&) = delete;
    task_scope(const worker_ref&&) = delete;
    // Destruction requires that no child is active and no join is pending;
    // violating that is a lifecycle contract violation and terminates. A
    // scope whose children all completed may be destroyed without a join; a
    // child failure nobody awaited is then routed to
    // report_unhandled_failure.
    ~task_scope();

    task_scope(const task_scope&) = delete;
    task_scope& operator=(const task_scope&) = delete;
    task_scope(task_scope&&) = delete;
    task_scope& operator=(task_scope&&) = delete;

    // Every operation is bound to this scope's stable address: child
    // completion nodes point back to it, and lazy join tasks borrow it.
    // Reject temporary scopes before either lifetime can be
    // created and keep even state/token access on the same explicit owner.
    void spawn(task<void> task) &;
    void spawn(task<void>) && = delete;
    // Stop control is worker-affine. Construction creates the stop state so
    // token() only observes it and never allocates or requests cancellation.
    void request_stop() & noexcept;
    void request_stop() && = delete;
    [[nodiscard]] stop_token token() const&;
    stop_token token() const&& = delete;
    [[nodiscard]] bool stop_requested() const& noexcept;
    bool stop_requested() const&& = delete;
    // Includes a spawn currently acquiring its child node.
    [[nodiscard]] std::size_t size() const& noexcept;
    std::size_t size() const&& = delete;
    [[nodiscard]] bool empty() const& noexcept {
        return size() == 0;
    }
    bool empty() const&& = delete;
    // One join: single-use, movable only before it is awaited. A cold join
    // does not close spawn admission; starting it claims the scope exactly
    // once and prevents further spawning. It waits for in-progress spawn
    // admission and child node retirement. The scope and worker must remain
    // alive until the started operation and all children have completed.
    class join_operation final {
    public:
        join_operation(const join_operation&) = delete;
        join_operation& operator=(const join_operation&) = delete;
        join_operation(join_operation&& other) noexcept;
        join_operation& operator=(join_operation&&) = delete;
        void operator&() const = delete;

        [[nodiscard]] bool await_ready();
        void await_suspend(std::coroutine_handle<> continuation) noexcept;
        void await_resume();

    private:
        friend class task_scope;

        explicit join_operation(task_scope& scope) noexcept
            : scope_(&scope) {}

        task_scope* scope_;
        bool started_{false};
    };

    // Creation and starting the join both require the bound worker. The join
    // rethrows the first child failure; each later child failure not
    // equivalent to it (see equivalent_failures) is routed to
    // report_unhandled_failure when that child completes.
    [[nodiscard]] join_operation join() &;
    join_operation join() && = delete;

private:
    struct node_type;
    struct retiring_guard;

    struct task_scope_open_type final {};
    class task_scope_joining_type final {
    public:
        explicit task_scope_joining_type(std::coroutine_handle<> continuation) noexcept
            : continuation_(continuation) {}

        [[nodiscard]] std::coroutine_handle<> continuation() const noexcept {
            return continuation_;
        }

    private:
        std::coroutine_handle<> continuation_;
    };
    struct task_scope_joined_type final {};

    static void child_complete(void* raw) noexcept;
    void finish(node_type* node) noexcept;
    void resume_join_if_idle() noexcept;
    void rethrow_failure();

    using lifecycle_type = std::variant<task_scope_open_type, task_scope_joining_type, task_scope_joined_type>;

    const worker_ref& worker_;
    stop_source stop_source_;
    std::size_t active_{0};
    retiring_guard* retiring_{nullptr};
    lifecycle_type lifecycle_;
    std::exception_ptr failure_{};
};

class scoped_capability_registration;

namespace detail {
class scoped_operation_registration;
}

// Worker-affine owner of borrowed capabilities and their lazy operations.
// Close discards cold frames before capability cleanup. Started operations must
// finish; close_and_join waits for their frames, leases and capability cleanup
// to retire, including when invoked reentrantly by a cleanup callback.
// bind_worker pins the scope to one worker: every operation in it then
// terminates when started, completed or retired cold on another thread.
class operation_scope final {
public:
    operation_scope() noexcept = default;
    ~operation_scope() {
        close();
    }

    operation_scope(const operation_scope&) = delete;
    operation_scope& operator=(const operation_scope&) = delete;
    operation_scope(operation_scope&&) = delete;
    operation_scope& operator=(operation_scope&&) = delete;

    void close() noexcept;
    void expire() noexcept;
    [[nodiscard]] task<void> close_and_join() &;
    task<void> close_and_join() && = delete;
    [[nodiscard]] bool active() const noexcept {
        return active_;
    }
    [[nodiscard]] bool has_pending_operations() const noexcept {
        return head_ != nullptr;
    }
    // Borrows the worker_ref, which must outlive this scope (the same rule
    // as task_scope). Bind once, before any operation is registered.
    void bind_worker(const worker_ref& worker) & noexcept {
        if (worker_ != nullptr || head_ != nullptr || capability_head_ != nullptr || !active_) {
            std::terminate();
        }
        worker_ = &worker;
    }
    void bind_worker(const worker_ref&) && = delete;
    void bind_worker(worker_ref&&) = delete;
    void bind_worker(const worker_ref&&) = delete;

private:
    friend class detail::scoped_operation_registration;
    friend class scoped_capability_registration;
    struct drain_guard;
    struct join_awaiter;
    void check_worker() const noexcept;
    void retire_cold_operations() noexcept;
    void expire_for_join() noexcept;
    void expire_capabilities() noexcept;
    void resume_joiner() noexcept;
    void link(detail::scoped_operation_registration& operation) noexcept;
    void unlink(detail::scoped_operation_registration& operation) noexcept;

    detail::scoped_operation_registration* head_{nullptr};
    scoped_capability_registration* capability_head_{nullptr};
    const worker_ref* worker_{nullptr};
    std::coroutine_handle<> join_continuation_{};
    bool active_{true};
    bool expiring_capabilities_{false};
    bool join_started_{false};
    bool join_complete_{false};
};

// Allocation-free value registration. An owner must explicitly rebind the
// cleanup target when copying or moving; ordinary copy/move is forbidden.
// Declare it after the payload so destruction unlinks before payload teardown.
class scoped_capability_registration final {
public:
    scoped_capability_registration() noexcept = default;
    scoped_capability_registration(operation_scope& scope, void* target,
        void (*cleanup)(void*) noexcept) noexcept;
    scoped_capability_registration(const scoped_capability_registration& other, void* target) noexcept;
    scoped_capability_registration(scoped_capability_registration&& other, void* target) noexcept;
    ~scoped_capability_registration();

    scoped_capability_registration(const scoped_capability_registration&) = delete;
    scoped_capability_registration& operator=(const scoped_capability_registration&) = delete;
    scoped_capability_registration(scoped_capability_registration&&) = delete;
    scoped_capability_registration& operator=(scoped_capability_registration&&) = delete;

    void require_active() const;
    [[nodiscard]] operation_scope& scope() const;
    void bind(operation_scope& scope, void* target, void (*cleanup)(void*) noexcept) noexcept;

private:
    friend class operation_scope;
    void link(operation_scope& scope) noexcept;
    void unlink() noexcept;
    void expire() noexcept;

    operation_scope* scope_{nullptr};
    scoped_capability_registration* previous_{nullptr};
    scoped_capability_registration* next_{nullptr};
    void* target_{nullptr};
    void (*cleanup_)(void*) noexcept {nullptr};
    bool active_{false};
};

template <typename t_type>
[[nodiscard]] task<t_type> make_scoped_operation(
    operation_scope& scope, task<t_type> task_value) {
    task_value.bind_scope(scope);
    return task_value;
}

// Intrusive, allocation-free wake primitive for one worker. Every operation
// which touches its waiter list is worker-affine; the worker_ref is therefore
// the dispatch target and the affinity capability, not an optional fast path.
class worker_signal final {
    enum class await_state_type : std::uint8_t {
        idle,
        linked,
        scheduled,
    };

public:
    class wait_operation final {
    public:
        wait_operation(const wait_operation&) = delete;
        wait_operation& operator=(const wait_operation&) = delete;
        // Only a cold operation may move; a linked waiter must stay address-stable.
        wait_operation(wait_operation&& other) noexcept
            : signal_(std::exchange(other.signal_, nullptr)) {
            if (other.started_) {
                std::terminate();
            }
        }
        wait_operation& operator=(wait_operation&&) = delete;
        void operator&() const = delete;
        ~wait_operation() {
            if (state_ != await_state_type::idle) {
                std::terminate();
            }
            release_reservation();
        }

        [[nodiscard]] bool await_ready() {
            if (signal_ == nullptr || started_) {
                throw std::logic_error("worker signal wait cannot be awaited");
            }
            started_ = true;
            try {
                signal_->require_current_worker();
            } catch (...) {
                release_reservation();
                throw;
            }
            if (!signal_->pending_) {
                return false;
            }
            signal_->pending_ = false;
            return true;
        }
        bool await_suspend(std::coroutine_handle<> value) noexcept {
            continuation_ = value;
            next_ = signal_->waiters_;
            state_ = await_state_type::linked;
            signal_->waiters_ = this;
            return true;
        }
        void await_resume() noexcept {
            release_reservation();
        }

    private:
        explicit wait_operation(worker_signal& signal) noexcept
            : signal_(&signal) {
            ++signal.reserved_waits_;
        }
        void release_reservation() noexcept {
            if (signal_ != nullptr) {
                --signal_->reserved_waits_;
                signal_ = nullptr;
            }
        }
        worker_signal* signal_;
        wait_operation* next_{nullptr};
        std::coroutine_handle<> continuation_{};
        await_state_type state_{await_state_type::idle};
        bool started_{false};
        friend class worker_signal;
    };

private:
    using awaiter_type = wait_operation;

public:
    explicit worker_signal(const worker_ref& worker_value)
        : worker_(require_worker(worker_value)) {}
    worker_signal(worker_ref&&) = delete;
    worker_signal(const worker_ref&&) = delete;

    ~worker_signal() {
        if (waiters_ != nullptr || scheduled_waiters_ != 0 || reserved_waits_ != 0) {
            std::terminate();
        }
    }

    worker_signal(const worker_signal&) = delete;
    worker_signal& operator=(const worker_signal&) = delete;

    // A cold single-use awaiter reserves the signal until completion or destruction.
    // Creation and starting the wait both require the bound worker.
    [[nodiscard]] wait_operation wait() & {
        require_current_worker();
        return wait_operation(*this);
    }
    wait_operation wait() && = delete;

    [[nodiscard]] const worker_ref& worker() const& noexcept {
        return worker_;
    }
    const worker_ref& worker() const&& = delete;

    // Wakes every waiter, or leaves one pending wake when none waits. Like
    // wait(), it must run on the bound worker and throws std::logic_error
    // elsewhere. A failed resume dispatch after waiters were detached has no
    // recoverable owner and terminates.
    void notify();

private:
    [[nodiscard]] static const worker_ref& require_worker(const worker_ref& worker_value) {
        if (!worker_value.valid()) {
            throw std::invalid_argument("worker signal requires a valid worker");
        }
        return worker_value;
    }

    void require_current_worker() const {
        if (!worker_.is_current()) {
            throw std::logic_error("worker signal operation must run on its worker");
        }
    }

    void resume_scheduled(awaiter_type* waiter, std::coroutine_handle<> continuation) noexcept;

    // The owning session/connection keeps its stable worker_ref alive until
    // every signal waiter and scheduled resumption has joined.
    const worker_ref& worker_;
    awaiter_type* waiters_{nullptr};
    std::size_t scheduled_waiters_{0};
    std::size_t reserved_waits_{0};
    bool pending_{false};
};

inline void worker_signal::notify() {
    require_current_worker();

    auto* waiter = std::exchange(waiters_, nullptr);
    if (waiter == nullptr) {
        pending_ = true;
        return;
    }

    while (waiter != nullptr) {
        auto* next_value = waiter->next_;
        const auto continuation = waiter->continuation_;
        waiter->next_ = nullptr;
        waiter->state_ = await_state_type::scheduled;
        ++scheduled_waiters_;
        // A detached intrusive node has no recoverable owner. Dispatch failure
        // is terminal instead of silently stranding the continuation. On the
        // worker's own thread this small fixed callable is posted directly.
        detail::worker_ref_access::defer_or_terminate(
            worker_, [this, waiter, continuation] { resume_scheduled(waiter, continuation); });
        waiter = next_value;
    }
}

inline void worker_signal::resume_scheduled(
    awaiter_type* waiter, std::coroutine_handle<> continuation) noexcept {
    if (!worker_.is_current() || waiter->state_ != await_state_type::scheduled ||
        waiter->continuation_ != continuation || scheduled_waiters_ == 0) {
        std::terminate();
    }
    --scheduled_waiters_;
    waiter->continuation_ = {};
    waiter->state_ = await_state_type::idle;
    continuation.resume();
}

enum class timer_sleep_result : std::uint8_t {
    elapsed,
    // The sleep was cut short because a stop was requested: the worker is
    // shutting down, or -- when a stop_token was passed -- whatever else owns
    // that token asked the work to stop. Named for the request rather than
    // for one of its causes, because a caller reacts to it the same way either
    // way: give up the rest of the wait.
    stop_requested,
};

class sleep_operation;

// Suspend the current coroutine on its bound worker for `duration`, or until
// `deadline`. The result distinguishes a normal elapsed delay from
// cancellation caused by worker shutdown (stop_timers): a sleep pending at
// that point, or started after the worker's timer queue stopped, returns
// stop_requested without throwing. The duration is the sleep's target, not
// an operation bound (operation_timeout): sleep_for(d) is sleep_until(now +
// d), so a non-positive duration, like a deadline that has passed, elapses
// without suspending. Durations saturate, so an effectively unbounded delay such as
// hours::max() waits for stop instead of overflowing.
//
// The sleep is also cut short when `stop_token_value` is stopped. A
// framework-provided wait that ignores the caller's stop token is a hole in
// every deadline built on that token, so anything a request can await takes
// one. The stop callback may run on a thread that is not this worker, and
// cancelling a worker timer is worker-owned state, so cancellation is deferred
// onto the worker and validated there -- a stop that arrives after the sleep
// already finished is a no-op rather than a use-after-free.
//
// The worker is borrowed for the lifetime of the returned operation: the
// caller must keep its address-stable worker_ref alive until the operation
// completes or is dropped cold. This is the same borrow-only boundary the
// request hot path applies everywhere else; a temporary worker_ref would dangle in
// the cold operation.
[[nodiscard]] sleep_operation sleep_for(
    const worker_ref& worker_value, timeout_duration duration, stop_token stop_token_value = {});
sleep_operation sleep_for(worker_ref&&, timeout_duration, stop_token = {}) = delete;
sleep_operation sleep_for(const worker_ref&&, timeout_duration, stop_token = {}) = delete;
[[nodiscard]] sleep_operation sleep_until(const worker_ref& worker_value,
    std::chrono::steady_clock::time_point deadline, stop_token stop_token_value = {});
sleep_operation sleep_until(
    worker_ref&&, std::chrono::steady_clock::time_point, stop_token = {}) = delete;
sleep_operation sleep_until(
    const worker_ref&&, std::chrono::steady_clock::time_point, stop_token = {}) = delete;

// One sleep on the caller's worker: single-use, movable only before it is
// awaited, and started on the borrowed worker by co_await. An empty stop token
// makes it a plain timer wait; a stoppable one also cancels the timer when stop
// is requested.
class sleep_operation final {
public:
    sleep_operation(const sleep_operation&) = delete;
    sleep_operation& operator=(const sleep_operation&) = delete;
    sleep_operation(sleep_operation&& other) noexcept;
    sleep_operation& operator=(sleep_operation&&) = delete;
    void operator&() const = delete;

    [[nodiscard]] bool await_ready();
    [[nodiscard]] bool await_suspend(std::coroutine_handle<> continuation);
    [[nodiscard]] timer_sleep_result await_resume() const noexcept;

private:
    friend sleep_operation sleep_for(const worker_ref&, timeout_duration, stop_token);
    friend sleep_operation sleep_until(
        const worker_ref&, std::chrono::steady_clock::time_point, stop_token);

    sleep_operation(const worker_ref& worker_value, timeout_duration duration,
        stop_token stop_token_value) noexcept
        : worker_(&worker_value),
          target_(duration),
          stop_token_(std::move(stop_token_value)) {}

    sleep_operation(const worker_ref& worker_value, std::chrono::steady_clock::time_point deadline,
        stop_token stop_token_value) noexcept
        : worker_(&worker_value),
          target_(deadline),
          stop_token_(std::move(stop_token_value)) {}

    const worker_ref* worker_;
    std::variant<timeout_duration, std::chrono::steady_clock::time_point> target_;
    stop_token stop_token_;
    std::coroutine_handle<> continuation_{};
    worker_timer_outcome outcome_{worker_timer_outcome::expired};
    bool started_{false};
    worker_timer_registration registration_;
    // Declared last so callback teardown completes before the timer registration
    // and the borrowed worker begin destruction.
    stop_registration stop_registration_;
};

// Offloading blocking work off a worker thread.
//
// A Ruvia worker is one thread driving one io_context, and every connection it
// accepted is dispatched on that thread. A handler that blocks -- hashing a
// password, calling a synchronous third-party SDK, rendering a template,
// touching a slow file -- freezes every other connection on that worker for as
// long as it blocks. Nothing about the coroutine machinery can hide that: the
// thread is simply not running the event loop any more.
//
// A blocking_pool is the escape hatch: a fixed set of ordinary threads with a
// bounded queue. run_blocking() hands the callable to that pool, suspends the
// calling coroutine, and resumes it on its own worker once the result comes
// back -- so the worker keeps serving other connections meanwhile.
//
// The callable runs on a foreign thread and outlives nothing: it must own
// everything it touches (capture by value or move), and it must not touch the
// context or any other worker-owned state. Nothing in C++
// enforces that -- the pool cannot see through a lambda's captures -- and a
// worker that stops while a task is still running does not wait for it, so a
// captured reference into a request is a use-after-free waiting to happen.
//
// A pool task must not wait on the same pool. The threads are a fixed set, so
// tasks that block until other tasks finish can occupy every one of them at
// once and deadlock -- the queue would drain only once a running task returns,
// and none can. Submitting without waiting is fine; waiting is not.

struct blocking_pool_options final {
    // 0 selects half of std::thread::hardware_concurrency(), clamped to 2..8.
    // Blocking work benefits from some oversubscription on small machines, but
    // the default must not create one process thread per logical CPU on large
    // hosts. Set an explicit value when the workload has different needs.
    std::size_t thread_count_{0};
    // Queued tasks waiting for a free thread. 0 selects thread_count * 64.
    // The queue is bounded on purpose: an unbounded one converts an overloaded
    // pool into unbounded memory growth and unbounded latency, and hides the
    // overload from the caller that could still shed load.
    std::size_t queue_capacity_{0};
};

enum class blocking_submit_status : std::uint8_t {
    accepted,
    queue_full,
    pool_stopped,
};

// A rejected submission returns the callable it did not run.
using blocking_submit_result =
    worker_delivery_result<move_only_function<void()>, blocking_submit_status, blocking_submit_status::accepted>;

struct blocking_pool_stats final {
    // Tasks waiting for a free thread, and tasks a thread is running right now.
    std::size_t queued_{0};
    std::size_t running_{0};
    // Tasks that ran to completion, whether or not the callable threw.
    std::uint64_t completed_{0};
    // Tasks refused because no thread and no queue slot were free. This is the
    // sizing signal: a growing count means thread_count or queue_capacity is too
    // small for the offered load.
    std::uint64_t rejected_{0};
    // Tasks that never ran because the pool was stopping -- dropped from the
    // queue by stop(), or submitted after it. Shutdown accounting, kept apart
    // from `rejected` so it cannot be mistaken for overload.
    std::uint64_t discarded_{0};
};

// Fixed threads, bounded queue. Construction starts the threads. Destruction
// stops accepting work, discards queued tasks, and detaches threads that are
// already running a callable; those callables may finish after the pool object
// is gone. Call join() explicitly when the owner must wait for every thread.
// Copy and move are deleted: submitters hold a reference.
class blocking_pool final {
public:
    explicit blocking_pool(blocking_pool_options options = {});
    ~blocking_pool();

    blocking_pool(const blocking_pool&) = delete;
    blocking_pool& operator=(const blocking_pool&) = delete;
    blocking_pool(blocking_pool&&) = delete;
    blocking_pool& operator=(blocking_pool&&) = delete;

    [[nodiscard]] std::size_t thread_count() const noexcept;
    [[nodiscard]] std::size_t queue_capacity() const noexcept;
    [[nodiscard]] blocking_pool_stats stats() const noexcept;

    // Safe from any thread. Admission is reserved before the callable moves
    // into the queue; a rejection (queue_full, or pool_stopped, including a
    // stop that lands during that move) returns the callable untouched in the
    // result, so the caller decides where it is destroyed. Callable moves and
    // destruction may query stats(), submit(), or stop(). Admission includes
    // submissions whose callable move has not finished.
    [[nodiscard]] blocking_submit_result submit(move_only_function<void()> task);

    // Stops accepting, discards tasks that have not started, and wakes the
    // threads. Tasks already running are not interrupted -- a blocking call
    // cannot be -- but nothing waits for their queued successors. Idempotent
    // and safe from any thread.
    void stop() noexcept;
    // Stops the pool and waits for its threads to leave their loops. Joining
    // from one of this pool's own threads would deadlock and throws logic_error
    // before changing the pool's state.
    // The destructor deliberately does not join already-running callables.
    void join();

private:
    struct impl_type;
    std::shared_ptr<impl_type> impl_;
};

enum class blocking_status : std::uint8_t {
    // The callable ran to completion. It may still have thrown: that is a
    // result, not a rejection.
    completed,
    // The pool's queue was full. The callable never ran.
    queue_full,
    // The pool was stopped before a thread picked the task up. Never ran.
    pool_stopped,
    // The worker stopped while the task was outstanding, so there is nobody
    // left to deliver a result to. The callable may or may not have run; its
    // result, if any, was discarded on the pool thread.
    worker_stopping,
    // The caller's stop token fired first. The callable may still be running;
    // its eventual result is discarded just like a timed-out result.
    cancelled,
    // The caller stopped waiting first. The callable keeps running -- a
    // blocking call cannot be interrupted -- and its result is discarded when
    // it finishes. This releases the caller's request, not the pool thread.
    timed_out,
    // The worker's event loop was given no blocking pool. The callable never ran.
    unavailable,
};

[[nodiscard]] std::string_view describe_blocking_status(blocking_status status) noexcept;

// Thrown when a blocking operation produced no result of its own: the pool
// refused it or the worker went away. A callable's own exception is rethrown
// unchanged instead -- it is the operation's result.
class blocking_operation_rejected final : public std::runtime_error {
public:
    [[nodiscard]] blocking_status status() const noexcept {
        return status_;
    }

private:
    template <typename>
    friend class blocking_result;

    explicit blocking_operation_rejected(blocking_status status);

    blocking_status status_;
};

namespace detail {

template <typename t_type>
struct blocking_payload final {
    using value_storage_type =
        std::conditional_t<std::is_void_v<t_type>, std::monostate, std::optional<t_type>>;

    blocking_status status_{blocking_status::completed};
    [[no_unique_address]] value_storage_type value_{};
    std::exception_ptr error_;
};

// Guards the one-shot half a submitted task owns. If the task is destroyed
// without running -- a stopped pool discards its queue -- the guard still
// answers the waiting coroutine instead of leaving it suspended forever.
// Answering or moving releases the completion, which is the answered marker:
// an empty completion rejects without effect. Holding nothing else keeps the
// submitted closure within move_only_function's inline storage.
template <typename t_type>
class blocking_completion_guard final {
public:
    explicit blocking_completion_guard(one_shot_completion<blocking_payload<t_type>> completion) noexcept
        : completion_(std::move(completion)) {}

    blocking_completion_guard(const blocking_completion_guard&) = delete;
    blocking_completion_guard& operator=(const blocking_completion_guard&) = delete;
    blocking_completion_guard(blocking_completion_guard&& other) noexcept = default;
    blocking_completion_guard& operator=(blocking_completion_guard&&) = delete;

    ~blocking_completion_guard() {
        blocking_payload<t_type> payload;
        payload.status_ = blocking_status::pool_stopped;
        // Nothing above this can receive a failure, and a rejected completion is
        // the normal case (the receiver may be gone), so the result is dropped.
        (void)completion_.complete(std::move(payload));
    }

    void answer(blocking_payload<t_type>&& payload_value) {
        const auto completion = std::move(completion_);
        try {
            (void)completion.complete(std::move(payload_value));
        } catch (...) {
            blocking_payload<t_type> failure;
            failure.error_ = std::current_exception();
            // An error-only payload has no result value to move, so it remains transportable
            // even when moving the callable's result was what failed above.
            (void)completion.complete(std::move(failure));
        }
    }

private:
    one_shot_completion<blocking_payload<t_type>> completion_;
};

}  // namespace detail

// A blocking operation's value, exception, or rejection before execution.
// The outcome category is fixed after construction. Moving a failed result
// preserves its failure in the source as well; it must not become a successful
// result without a value. Replace an owning optional by emplacing a new result.
// with_value(), with_error() and with_status() build the same outcomes for a
// composed offload.
template <typename t_type>
class blocking_result final {
public:
    blocking_result(const blocking_result&) = delete;
    blocking_result& operator=(const blocking_result&) = delete;
    blocking_result(blocking_result&& other) noexcept(
        std::is_nothrow_move_constructible_v<detail::blocking_payload<t_type>>)
        requires std::is_move_constructible_v<detail::blocking_payload<t_type>>
        : payload_(std::move(other.payload_)) {
        if (payload_.error_ != nullptr) {
            other.payload_.error_ = payload_.error_;
        }
    }
    blocking_result& operator=(blocking_result&&) = delete;

    [[nodiscard]] blocking_status status() const noexcept {
        return payload_.status_;
    }

    // True when execution and result delivery both completed without error.
    [[nodiscard]] bool completed() const noexcept {
        return payload_.status_ == blocking_status::completed && payload_.error_ == nullptr;
    }

    // Execution or result delivery failed; value() rethrows the exception.
    [[nodiscard]] bool failed() const noexcept {
        return payload_.error_ != nullptr;
    }

    [[nodiscard]] const std::exception_ptr& error() const& noexcept {
        return payload_.error_;
    }
    const std::exception_ptr& error() const&& = delete;

    // Rethrows the stored error, or throws blocking_operation_rejected if the
    // operation was rejected.
    decltype(auto) value() const& {
        rethrow_if_not_completed();
        if constexpr (!std::is_void_v<t_type>) {
            return static_cast<const t_type&>(*payload_.value_);
        }
    }

    void value() && = delete;
    void value() const&& = delete;

    decltype(auto) value() & {
        rethrow_if_not_completed();
        if constexpr (!std::is_void_v<t_type>) {
            return static_cast<t_type&>(*payload_.value_);
        }
    }

    t_type take_value() && {
        rethrow_if_not_completed();
        if constexpr (!std::is_void_v<t_type>) {
            return std::move(*payload_.value_);
        }
    }

    // The callable completed with this value.
    template <typename u_type>
        requires(!std::is_void_v<t_type> && std::constructible_from<t_type, u_type&&>)
    [[nodiscard]] static blocking_result with_value(u_type&& value) {
        blocking_result result_value(blocking_status::completed);
        result_value.payload_.value_.emplace(std::forward<u_type>(value));
        return result_value;
    }

    [[nodiscard]] static blocking_result with_value() noexcept
        requires std::is_void_v<t_type>
    {
        return blocking_result(blocking_status::completed);
    }

    // Execution or result delivery failed with this non-null exception.
    [[nodiscard]] static blocking_result with_error(std::exception_ptr error) {
        if (error == nullptr) {
            throw std::invalid_argument("blocking error result requires an exception");
        }
        blocking_result result_value(blocking_status::completed);
        result_value.payload_.error_ = std::move(error);
        return result_value;
    }

    // The operation produced no result of its own; every status but completed.
    [[nodiscard]] static blocking_result with_status(blocking_status status) {
        if (status == blocking_status::completed) {
            throw std::invalid_argument("blocking status result requires a rejection status");
        }
        return blocking_result(status);
    }

private:
    explicit blocking_result(blocking_status status) noexcept {
        payload_.status_ = status;
    }

    void rethrow_if_not_completed() const {
        if (payload_.error_ != nullptr) {
            std::rethrow_exception(payload_.error_);
        }
        if (payload_.status_ != blocking_status::completed) {
            throw blocking_operation_rejected(payload_.status_);
        }
        if constexpr (!std::is_void_v<t_type>) {
            if (!payload_.value_.has_value()) {
                std::terminate();
            }
        }
    }

    detail::blocking_payload<t_type> payload_;
};

namespace detail {

// A completed payload always holds a value or an error.
template <typename t_type>
[[nodiscard]] blocking_result<t_type> completed_blocking_result(blocking_payload<t_type>&& payload_value) {
    if (payload_value.error_ != nullptr) {
        return blocking_result<t_type>::with_error(std::move(payload_value.error_));
    }
    if constexpr (std::is_void_v<t_type>) {
        return blocking_result<t_type>::with_value();
    } else {
        return blocking_result<t_type>::with_value(std::move(*payload_value.value_));
    }
}

}  // namespace detail

// Runs `fn` on the blocking pool the worker's event loop borrows and resumes
// the caller on `worker` with a typed result. Queue pressure, a worker without
// a pool, shutdown, cancellation and timeout remain inspectable statuses; the
// callable's own exception is retained in the result.
//
// The returned task must be awaited on `worker` -- that is the thread the
// coroutine resumes on and the only thread the result is touched from;
// awaiting it elsewhere throws std::logic_error before `fn` is submitted. `fn`
// is moved into the pool and runs on a foreign thread: it must own everything
// it uses (see the file header).
//
// options.timeout_ bounds the caller, not the work: a blocking call cannot be
// interrupted, so the pool thread stays occupied until `fn` returns. Use it to
// keep one wedged dependency from pinning a request -- its connection, its
// arena, its leases -- indefinitely. A relative timeout starts when the task
// starts; an absolute one shares a deadline with the caller's other phases.
// options.stop_token_ cancels the wait the same way.
//
// Never throws for a rejection; the status says what happened. A worker
// without a pool, a stop token that is already stopped, or a deadline that has
// passed rejects without submitting `fn`. Use std::move(result).take_value()
// to turn a rejection into an exception and to rethrow the callable's own
// exception.
template <typename fn_type>
[[nodiscard]] auto run_blocking(worker_ref worker_value, operation_options options, fn_type fn)
    -> task<blocking_result<std::invoke_result_t<fn_type&>>> {
    using result_type = std::invoke_result_t<fn_type&>;
    using payload = detail::blocking_payload<result_type>;
    using outcome_type = blocking_result<result_type>;
    const operation_deadline deadline(options.timeout_);
    auto stop_token_value = std::move(options.stop_token_);
    static_assert(std::is_void_v<result_type> || std::is_move_constructible_v<result_type>,
        "a blocking callable's result travels back to the worker by move");

    // Every outcome the wait would decide without the callable is decided
    // first, so a misplaced, cancelled, or expired call never occupies a pool
    // thread or queue slot for a result nobody receives.
    if (!worker_value.is_current()) {
        throw std::logic_error("blocking work must be awaited on its bound worker");
    }
    auto* const pool = worker_value.blocking_pool();
    if (pool == nullptr) {
        co_return outcome_type::with_status(blocking_status::unavailable);
    }
    if (stop_token_value.stop_requested()) {
        co_return outcome_type::with_status(blocking_status::cancelled);
    }
    if (deadline.expired()) {
        co_return outcome_type::with_status(blocking_status::timed_out);
    }
    if (!worker_value.accepting()) {
        co_return outcome_type::with_status(blocking_status::worker_stopping);
    }

    // A worker that is already stopping refuses the registration. That is the
    // shutdown race, not a failure of this call, so it becomes the status a
    // shutdown always produces -- an offload must never turn into an exception
    // the caller did not ask for.
    std::optional<std::pair<one_shot_completion<payload>, one_shot_receiver<payload>>> channel;
    try {
        channel.emplace(make_one_shot<payload>(std::move(worker_value)));
    } catch (const std::runtime_error&) {
        co_return outcome_type::with_status(blocking_status::worker_stopping);
    }
    auto& [completion, receiver] = *channel;
    const auto submitted =
        pool->submit([guard = detail::blocking_completion_guard<result_type>(std::move(completion)),
                         call = std::move(fn)]() mutable {
            payload payload_value;
            try {
                if constexpr (std::is_void_v<result_type>) {
                    call();
                } else {
                    payload_value.value_.emplace(call());
                }
            } catch (...) {
                payload_value.error_ = std::current_exception();
            }
            guard.answer(std::move(payload_value));
        });
    if (!submitted.accepted()) {
        // The returned closure is destroyed here; its guard answers the
        // one-shot with pool_stopped, which nobody awaits any more.
        co_return outcome_type::with_status(submitted.status() == blocking_submit_status::queue_full
                                                ? blocking_status::queue_full
                                                : blocking_status::pool_stopped);
    }

    try {
        auto waited = co_await receiver.wait({.timeout_ = deadline.timeout(), .stop_token_ = std::move(stop_token_value)});
        if (waited.has_value()) {
            auto& payload_value = waited.value();
            if (payload_value.status_ != blocking_status::completed) {
                co_return outcome_type::with_status(payload_value.status_);
            }
            co_return detail::completed_blocking_result(std::move(payload_value));
        }
        if (waited.status() == worker_wait_status::timed_out) {
            co_return outcome_type::with_status(blocking_status::timed_out);
        }
        if (waited.status() == worker_wait_status::cancelled) {
            co_return outcome_type::with_status(blocking_status::cancelled);
        }
        // Closed cannot happen -- this coroutine owns the receiver and is the only
        // waiter -- so anything else is the worker going away under the operation.
        co_return outcome_type::with_status(blocking_status::worker_stopping);
    } catch (...) {
        // Payload delivery now moves the result on the receiving worker.
        co_return outcome_type::with_error(std::current_exception());
    }
}

template <typename fn_type>
[[nodiscard]] auto run_blocking(worker_ref worker_value, fn_type fn)
    -> task<blocking_result<std::invoke_result_t<fn_type&>>> {
    return run_blocking(std::move(worker_value), operation_options{}, std::move(fn));
}

// The completion argument of async_start(). Void tasks complete with
// std::exception_ptr, which satisfies Asio's disposition concept directly;
// value-returning tasks complete with the value first and the failure second,
// passed by rvalue reference so over-aligned results are never passed by value
// on MSVC.
template <typename t_type>
    requires detail::asio_task_result<t_type>
using task_completion_result = std::conditional_t<std::is_void_v<t_type>, std::exception_ptr,
    std::variant<t_type, std::exception_ptr>>;

}  // namespace ruvia

namespace ruvia::detail {

// Owns the started task and the completion handler, allocated with the
// handler's associated allocator, until delivery on its associated executor.
template <typename t_type, typename handler_type>
class task_completion_state final {
public:
    using handler_allocator_type = asio::associated_allocator_t<handler_type>;
    using state_allocator_type = typename std::allocator_traits<handler_allocator_type>::template rebind_alloc<
        task_completion_state>;
    using state_allocator_traits_type = std::allocator_traits<state_allocator_type>;

    static task_completion_state* create(task<t_type>&& task_value, handler_type&& handler_value) {
        handler_allocator_type handler_allocator(asio::get_associated_allocator(handler_value));
        state_allocator_type state_allocator(handler_allocator);
        auto* state_value = state_allocator_traits_type::allocate(state_allocator, 1);
        try {
            state_allocator_traits_type::construct(state_allocator, state_value, std::move(task_value),
                std::forward<handler_type>(handler_value), std::move(handler_allocator));
        } catch (...) {
            state_allocator_traits_type::deallocate(state_allocator, state_value, 1);
            throw;
        }
        return state_value;
    }

    task_completion_state(
        task<t_type>&& task_value, handler_type&& handler_value, handler_allocator_type allocator_value)
        : task_(std::move(task_value)),
          handler_(std::forward<handler_type>(handler_value)),
          allocator_(std::move(allocator_value)) {}

    void start() {
        task_.coroutine_.promise().control_.set_completion(this, &task_completion_state::complete);
        task_.start();
    }

    static void complete(void* raw) noexcept {
        auto* state_value = static_cast<task_completion_state*>(raw);
        auto executor = asio::get_associated_executor(state_value->handler_);
        auto completion_allocator = state_value->allocator_;
        state_owner_type owner_value(state_value, state_deleter_type{state_allocator_type(state_value->allocator_)});
        asio::post(executor, asio::bind_allocator(std::move(completion_allocator),
                                 [owner_value = std::move(owner_value)]() mutable {
                                     deliver(std::move(owner_value));
                                 }));
    }

private:
    struct state_deleter_type final {
        void operator()(task_completion_state* state_value) noexcept {
            state_allocator_traits_type::destroy(allocator_, state_value);
            state_allocator_traits_type::deallocate(allocator_, state_value, 1);
        }

        state_allocator_type allocator_;
    };

    using state_owner_type = std::unique_ptr<task_completion_state, state_deleter_type>;

    static void deliver(state_owner_type owner_value) {
        // The delivery owner outlives the result and every callback argument.
        // An owning completion handler can publish external readiness in its
        // destructor only after moved-from result storage has been reclaimed.
        auto handler = std::move(owner_value->handler_);
        owner_value->task_.prepare_direct_completion();
        auto result_value = [&owner_value]() -> task_completion_result<t_type> {
            try {
                if constexpr (std::is_void_v<t_type>) {
                    owner_value->task_.coroutine_.promise().result();
                    return {};
                } else {
                    return task_completion_result<t_type>(
                        std::in_place_index<0>, owner_value->task_.coroutine_.promise().result());
                }
            } catch (...) {
                if constexpr (std::is_void_v<t_type>) {
                    return std::current_exception();
                } else {
                    return task_completion_result<t_type>(std::in_place_index<1>, std::current_exception());
                }
            }
        }();
        owner_value->task_.finish_direct_completion();
        owner_value.reset();
        std::move(handler)(std::move(result_value));
    }

    task<t_type> task_;
    handler_type handler_;
    handler_allocator_type allocator_;
};

template <typename t_type>
    requires asio_task_result<t_type> && (!std::is_void_v<t_type>)
[[nodiscard]] asio::awaitable<t_type> await_non_void_task(task<t_type> task_value) {
    auto result_value = co_await async_start(std::move(task_value), asio::use_awaitable);
    if (result_value.index() == 1) {
        std::rethrow_exception(std::get<1>(result_value));
    }
    co_return std::move(std::get<0>(result_value));
}

}  // namespace ruvia::detail

namespace ruvia {

// Starts a lazy task as an Asio asynchronous operation. The task starts
// inline on the calling thread; its completion is delivered through the
// handler's associated executor with a task_completion_result<T>, after the
// coroutine frame has been reclaimed. The operation state is allocated with
// the handler's associated allocator. Any completion token works: a callback,
// use_awaitable, use_future, deferred, or a custom token.
template <typename t_type, typename completion_token_type>
    requires detail::asio_task_result<t_type>
auto async_start(task<t_type>&& task_value, completion_token_type&& token) {
    task_value.prepare_direct_start();
    using completion_signature_type =
        std::conditional_t<std::is_void_v<t_type>, void(std::exception_ptr), void(task_completion_result<t_type>&&)>;
    return asio::async_initiate<completion_token_type, completion_signature_type>(
        [](auto&& handler, task<t_type> started_task) {
            using handler_type = std::decay_t<decltype(handler)>;
            auto* state_value = detail::task_completion_state<t_type, handler_type>::create(
                std::move(started_task), std::forward<decltype(handler)>(handler));
            state_value->start();
        },
        token, std::move(task_value));
}

// Adapts a lazy task for an Asio coroutine owner such as co_spawn. A failed
// task rethrows its exception from the awaitable. Void tasks use Asio's
// std::exception_ptr disposition, so use_awaitable produces the awaitable<void>
// directly without a wrapper coroutine frame.
template <typename t_type>
    requires detail::asio_task_result<t_type>
[[nodiscard]] asio::awaitable<t_type> as_awaitable(task<t_type> task_value) {
    if constexpr (std::is_void_v<t_type>) {
        return async_start(std::move(task_value), asio::use_awaitable);
    } else {
        return detail::await_non_void_task(std::move(task_value));
    }
}

// Asio completion signatures always provide an error code and may also provide
// a result that remains meaningful on partial failure (for example transferred
// bytes). The completion therefore owns both fields. Void completions carry
// only the error code: the result accessors and the two-argument factory exist
// only for a non-void result_type.
template <typename result_type>
class asio_completion final {
    using result_storage_type = std::conditional_t<std::is_void_v<result_type>, std::monostate, result_type>;

public:
    asio_completion(const asio_completion&) = default;
    asio_completion(asio_completion&&) = default;
    asio_completion& operator=(const asio_completion&) = delete;
    asio_completion& operator=(asio_completion&&) = delete;

    [[nodiscard]] static asio_completion completed(std::error_code error_code) noexcept
        requires std::is_void_v<result_type>
    {
        return asio_completion(error_code, result_storage_type{});
    }

    [[nodiscard]] static asio_completion completed(std::error_code error_code, result_storage_type result_value)
        requires(!std::is_void_v<result_type>)
    {
        return asio_completion(error_code, std::move(result_value));
    }

    [[nodiscard]] std::error_code error_code() const noexcept {
        return error_code_;
    }

    [[nodiscard]] bool succeeded() const noexcept {
        return !error_code_;
    }

    [[nodiscard]] bool failed() const noexcept {
        return static_cast<bool>(error_code_);
    }

    [[nodiscard]] const result_storage_type& result() const& noexcept
        requires(!std::is_void_v<result_type>)
    {
        return result_;
    }
    const result_storage_type& result() const&& = delete;

    [[nodiscard]] result_storage_type& result() & noexcept
        requires(!std::is_void_v<result_type>)
    {
        return result_;
    }
    result_storage_type& result() && = delete;

    [[nodiscard]] result_storage_type take_result() &&
        requires(!std::is_void_v<result_type>)
    {
        return std::move(result_);
    }

private:
    asio_completion(std::error_code error_code, result_storage_type&& result_value) noexcept(
        std::is_nothrow_move_constructible_v<result_storage_type>)
        : error_code_(error_code),
          result_(std::move(result_value)) {}

    std::error_code error_code_;
    [[no_unique_address]] result_storage_type result_;
};

// The single-use awaiter of one completion-based Asio operation, as returned
// by async_asio(). It owns the initiation and the completion state until the
// operation finishes, and is neither copyable, movable, nor addressable; an
// owner that keeps it beyond one await-expression constructs it in place
// (for example std::optional::emplace) from the initiation.
template <typename result_type, typename initiate_type>
class asio_completion_awaiter final {
public:
    explicit asio_completion_awaiter(initiate_type initiate)
        : initiate_(std::move(initiate)) {}
    asio_completion_awaiter(const asio_completion_awaiter&) = delete;
    asio_completion_awaiter& operator=(const asio_completion_awaiter&) = delete;
    void operator&() const = delete;

    [[nodiscard]] bool await_ready() const noexcept {
        return false;
    }

    // If initiate_ throws, the exception propagates from the await-expression
    // directly ([expr.await]/5), without an exception_ptr side channel.
    //
    // An asio initiation may complete synchronously (for example a closed
    // socket or a resolver cache hit), invoking the completion handler before
    // await_suspend returns. Resuming the continuation from inside that window
    // would run the coroutine while it is still suspended-by-await_suspend and
    // potentially destroy its frame before this function returns. The
    // suspend_race_state records the completion instead; the return value of
    // await_suspend then reports the actual race order, so a synchronous
    // completion is consumed by await_resume without resuming at all.
    [[nodiscard]] bool await_suspend(std::coroutine_handle<> continuation) {
        if constexpr (std::is_void_v<result_type>) {
            initiate_([this, continuation](std::error_code ec, auto&&...) mutable {
                if (state_.complete(asio_completion<void>::completed(ec))) {
                    continuation.resume();
                }
            });
        } else {
            initiate_([this, continuation](std::error_code ec, result_type result_value) mutable {
                if (state_.complete(asio_completion<result_type>::completed(ec, std::move(result_value)))) {
                    continuation.resume();
                }
            });
        }
        return state_.suspend(continuation);
    }

    [[nodiscard]] asio_completion<result_type> await_resume() {
        return state_.take_value();
    }

private:
    initiate_type initiate_;
    detail::suspend_race_state<asio_completion<result_type>> state_;
};

// Adapts a completion-based Asio operation for a Ruvia coroutine. The
// initiation receives one handler taking (std::error_code[, result_type]).
template <typename result_type = void, typename initiate_type>
[[nodiscard]] auto async_asio(initiate_type&& initiate) {
    return asio_completion_awaiter<result_type, std::decay_t<initiate_type>>(std::forward<initiate_type>(initiate));
}

class event_loop;

namespace detail {

// Routes an unobserved root-task failure through the loop's own failure sink,
// borrowed from the live loop state instead of copied into every task. A
// retired loop falls back to the unhandled-failure diagnostic.
void report_root_task_failure(const std::weak_ptr<event_loop_state>& loop, std::exception_ptr failure) noexcept;

class root_task_completion final {
public:
    root_task_completion(worker_ref worker_value, std::weak_ptr<event_loop_state> loop_value) noexcept
        : worker_(std::move(worker_value)),
          loop_(std::move(loop_value)) {}

    root_task_completion(const root_task_completion&) = delete;
    root_task_completion& operator=(const root_task_completion&) = delete;

    [[nodiscard]] bool valid() const noexcept {
        const std::lock_guard lock(mutex_);
        return owner_alive_;
    }

    [[nodiscard]] bool ready() const noexcept {
        const std::lock_guard lock(mutex_);
        return complete_;
    }

    void wait() const {
        require_foreign_thread();
        std::unique_lock lock(mutex_);
        completed_.wait(lock, [this] { return complete_; });
    }

    [[nodiscard]] bool wait_until(std::chrono::steady_clock::time_point deadline) const {
        require_foreign_thread();
        std::unique_lock lock(mutex_);
        return completed_.wait_until(lock, deadline, [this] { return complete_; });
    }

    void abandon() noexcept {
        std::exception_ptr unobserved;
        {
            const std::lock_guard lock(mutex_);
            if (!owner_alive_) {
                return;
            }
            owner_alive_ = false;
            if (complete_) {
                unobserved = failure_;
            }
        }
        report(std::move(unobserved));
    }

    void stage_failure(std::exception_ptr failure) noexcept {
        const std::lock_guard lock(mutex_);
        if (complete_) {
            std::terminate();
        }
        failure_ = std::move(failure);
    }

    // Publish only after the delivery owner has destroyed all intermediate
    // result objects and reclaimed their moved-from storage.
    void publish_completion() noexcept {
        std::exception_ptr unobserved;
        {
            const std::lock_guard lock(mutex_);
            if (complete_) {
                std::terminate();
            }
            complete_ = true;
            if (!owner_alive_) {
                unobserved = failure_;
            }
        }
        completed_.notify_all();
        report(std::move(unobserved));
    }

    void complete_failure(std::exception_ptr failure) noexcept {
        stage_failure(std::move(failure));
        publish_completion();
    }

    [[nodiscard]] std::exception_ptr consume_failure() {
        wait();
        std::lock_guard lock(mutex_);
        if (!owner_alive_) {
            throw std::logic_error("root task result was already consumed");
        }
        owner_alive_ = false;
        return failure_;
    }

private:
    void require_foreign_thread() const {
        if (worker_.is_current()) {
            throw std::logic_error("cannot wait for a root task on its event loop");
        }
    }

    void report(std::exception_ptr failure) const noexcept {
        if (!failure) {
            return;
        }
        report_root_task_failure(loop_, std::move(failure));
    }

    worker_ref worker_;
    std::weak_ptr<event_loop_state> loop_;
    mutable std::mutex mutex_;
    mutable std::condition_variable completed_;
    std::exception_ptr failure_;
    bool complete_{false};
    bool owner_alive_{true};
};

struct root_task_void_result final {};

template <typename t_type>
class root_task_state final {
public:
    root_task_state(worker_ref worker_value, std::weak_ptr<event_loop_state> loop_value) noexcept
        : completion_(std::move(worker_value), std::move(loop_value)) {}

    // The type-independent completion protocol; only the result is typed here.
    [[nodiscard]] root_task_completion& completion() noexcept {
        return completion_;
    }
    [[nodiscard]] const root_task_completion& completion() const noexcept {
        return completion_;
    }

    template <typename value_type>
        requires std::same_as<std::remove_cvref_t<value_type>, std::remove_cv_t<t_type>>
    void stage_value(value_type&& value) {
        value_.emplace(std::forward<value_type>(value));
    }

    void stage_value() noexcept
        requires std::is_void_v<t_type>
    {}

    t_type get() {
        auto failure = completion_.consume_failure();
        if (failure) {
            std::rethrow_exception(failure);
        }
        if constexpr (!std::is_void_v<t_type>) {
            struct retire_value final {
                std::optional<t_type>& value_;
                ~retire_value() {
                    value_.reset();
                }
            } retire{value_};
            return std::move(*value_);
        }
    }

private:
    root_task_completion completion_;
    [[no_unique_address]] std::conditional_t<std::is_void_v<t_type>, root_task_void_result, std::optional<t_type>> value_;
};

}  // namespace detail

template <typename t_type>
class [[nodiscard]] root_task final {
public:
    root_task(const root_task&) = delete;
    root_task& operator=(const root_task&) = delete;

    root_task(root_task&& other) noexcept
        : state_(std::move(other.state_)) {}

    root_task& operator=(root_task&& other) noexcept {
        if (this != &other) {
            // The old result may own or replace the source during retirement.
            root_task incoming(std::move(other));
            state_.swap(incoming.state_);
        }
        return *this;
    }

    ~root_task() {
        reset();
    }

    [[nodiscard]] bool valid() const noexcept {
        return state_ && state_->completion().valid();
    }

    // True once the task has completed and its result has not been consumed.
    [[nodiscard]] bool ready() const noexcept {
        return state_ && state_->completion().valid() && state_->completion().ready();
    }

    // Blocks a thread other than the loop's own until completion.
    void wait() const {
        require_state().completion().wait();
    }

    // Bounded wait(): returns whether the task completed in time. A relative
    // bound starts now; an absolute deadline that has passed only reports the
    // current state.
    [[nodiscard]] bool wait_for(operation_timeout timeout) const {
        return require_state().completion().wait_until(timeout.deadline_from(std::chrono::steady_clock::now()));
    }

    // Consumes the result, so only an rvalue extracts it:
    // std::move(root).get(). User result moves run without the state mutex
    // and may re-enter this root_task. The object must remain alive for the
    // entire synchronous call; replacing its owned state during a move is
    // supported.
    decltype(auto) get() && {
        if (!state_) {
            throw std::logic_error("root task has no result");
        }
        auto state_value = state_;
        struct retire_state final {
            std::shared_ptr<detail::root_task_state<t_type>>& state_;
            const std::shared_ptr<detail::root_task_state<t_type>>& expected_;
            ~retire_state() {
                if (state_ == expected_ && !expected_->completion().valid()) {
                    state_.reset();
                }
            }
        } retire{state_, state_value};
        if constexpr (std::is_void_v<t_type>) {
            state_value->get();
            return;
        } else {
            return state_value->get();
        }
    }
    void get() & = delete;

private:
    explicit root_task(std::shared_ptr<detail::root_task_state<t_type>> state_value) noexcept
        : state_(std::move(state_value)) {}

    [[nodiscard]] const detail::root_task_state<t_type>& require_state() const {
        if (!state_) {
            throw std::logic_error("root task has no state");
        }
        return *state_;
    }

    void reset() noexcept {
        if (auto state_value = std::exchange(state_, nullptr)) {
            state_value->completion().abandon();
        }
    }

    std::shared_ptr<detail::root_task_state<t_type>> state_;
    friend class event_loop;
};

namespace detail {
class event_loop_stop_listener;
template <typename t_type>
struct root_task_launch_guard;
}  // namespace detail

// Thrown by event_loop::start() when the loop refuses a task: status()
// distinguishes a full admission queue from a stopping or retired loop. Also
// the failure of an admitted root task whose launch was discarded because
// the loop stopped before running it.
class event_loop_start_rejected final : public std::runtime_error {
public:
    [[nodiscard]] post_status status() const noexcept {
        return status_;
    }

private:
    friend class event_loop;
    template <typename>
    friend struct detail::root_task_launch_guard;

    explicit event_loop_start_rejected(post_status status);

    post_status status_;
};

// The outcome of event_loop::try_start(): accepted owns the started
// root_task (value()/take_value()); a rejection owns the unconsumed cold task
// (rejected()/take_rejected()) with its post_status.
template <typename t_type>
using event_loop_start_result =
    worker_delivery_result<task<t_type>, post_status, post_status::accepted, root_task<t_type>>;

namespace detail {

// drain() destroys remaining queue closures without invoking them when
// a posted task throws. Own that destructor path so wait()/get() cannot
// hang and an unobserved failure still reaches the loop sink.
template <typename t_type>
struct root_task_launch_guard final {
    std::optional<task<t_type>> task_;
    std::shared_ptr<detail::root_task_state<t_type>> completion_;

    root_task_launch_guard(task<t_type> task_value, std::shared_ptr<detail::root_task_state<t_type>> state_value) noexcept
        : task_(std::move(task_value)),
          completion_(std::move(state_value)) {}
    root_task_launch_guard(const root_task_launch_guard&) = delete;
    root_task_launch_guard& operator=(const root_task_launch_guard&) = delete;
    root_task_launch_guard(root_task_launch_guard&& other) noexcept
        : task_(std::move(other.task_)),
          completion_(std::move(other.completion_)) {}
    root_task_launch_guard& operator=(root_task_launch_guard&&) = delete;
    ~root_task_launch_guard() {
        // A rejected or discarded launch still owns a cold frame.
        // Retire it before waking a caller that may reclaim its inputs.
        task_.reset();
        if (completion_ != nullptr) {
            completion_->completion().complete_failure(
                std::make_exception_ptr(event_loop_start_rejected(post_status::worker_stopping)));
        }
    }
    void release() noexcept {
        task_.reset();
        completion_.reset();
    }
};
template <typename t_type>
struct root_task_completion_handler final {
    using executor_type = asio::io_context::executor_type;

    executor_type executor_;
    std::shared_ptr<detail::root_task_state<t_type>> completion_;
    std::shared_ptr<void> retirement_lease_;
    bool delivered_{false};

    root_task_completion_handler(executor_type executor_value, std::shared_ptr<detail::root_task_state<t_type>> state_value,
        std::shared_ptr<void> lease_value) noexcept
        : executor_(std::move(executor_value)),
          completion_(std::move(state_value)),
          retirement_lease_(std::move(lease_value)) {}
    root_task_completion_handler(const root_task_completion_handler&) = delete;
    root_task_completion_handler& operator=(const root_task_completion_handler&) = delete;
    root_task_completion_handler(root_task_completion_handler&& other) noexcept
        : executor_(std::move(other.executor_)),
          completion_(std::move(other.completion_)),
          retirement_lease_(std::move(other.retirement_lease_)),
          delivered_(std::exchange(other.delivered_, false)) {}
    root_task_completion_handler& operator=(root_task_completion_handler&&) = delete;
    ~root_task_completion_handler() {
        if (delivered_) {
            completion_->completion().publish_completion();
        }
    }
    [[nodiscard]] executor_type get_executor() const noexcept {
        return executor_;
    }
    void operator()(task_completion_result<t_type>&& result_value) noexcept {
        try {
            if constexpr (std::is_void_v<t_type>) {
                if (result_value != nullptr) {
                    completion_->completion().stage_failure(std::move(result_value));
                } else {
                    completion_->stage_value();
                }
            } else if (result_value.index() == 1) {
                completion_->completion().stage_failure(std::get<1>(result_value));
            } else {
                completion_->stage_value(std::move(std::get<0>(result_value)));
            }
        } catch (...) {
            completion_->completion().stage_failure(std::current_exception());
        }
        delivered_ = true;
    }
};

}  // namespace detail

class event_loop_stop_registration final {
public:
    event_loop_stop_registration() noexcept = default;
    ~event_loop_stop_registration();

    event_loop_stop_registration(const event_loop_stop_registration&) = delete;
    event_loop_stop_registration& operator=(const event_loop_stop_registration&) = delete;
    event_loop_stop_registration(event_loop_stop_registration&&) noexcept = default;
    event_loop_stop_registration& operator=(event_loop_stop_registration&& other) noexcept;

    [[nodiscard]] bool valid() const noexcept;
    void reset() noexcept;

private:
    explicit event_loop_stop_registration(
        std::shared_ptr<detail::event_loop_stop_listener> listener_value) noexcept;

    std::shared_ptr<detail::event_loop_stop_listener> listener_;
    friend class event_loop;
};

// A stable handle to one Ruvia runtime and its Asio execution context. event_loop
// does not own a thread: event_loop_pool or event_loop_attachment owns and drives
// the runtime that produced it.
class event_loop final {
public:
    event_loop() noexcept = default;

    [[nodiscard]] bool valid() const noexcept;

    // A retired pool loop or attachment is invalid; accessing its context or
    // obtaining a fresh executor then throws std::logic_error. Previously
    // obtained executors must not outlive the loop's physical context storage.
    [[nodiscard]] asio::io_context& io_context() const&;
    asio::io_context& io_context() const&& = delete;
    [[nodiscard]] asio::io_context::executor_type executor() const;
    // The loop's worker endpoint: admission state, identity, posting, timers,
    // and the completion lane. Rvalue loops return a copy: the worker_ref must not
    // outlive its loop owner.
    [[nodiscard]] const worker_ref& worker() const& noexcept;
    [[nodiscard]] worker_ref worker() const&& noexcept;

    // Reports a terminal runtime failure without throwing on the caller's
    // thread. Pool owners retain the first failure for join(); attachments
    // route it to their configured failure callback or the unhandled-failure
    // diagnostic, then retire without throwing from the host's run().
    void report_failure(std::exception_ptr failure) const noexcept;

    // Loop-level cold-path cleanup of an already owned resource; it belongs
    // to no admitted operation and is not new business work. Bypasses bounded
    // admission but takes retirement protection, so it can still return false
    // after retirement has begun. Accepted callbacks and their owned inputs
    // retire before the loop can release its execution context. Contrast
    // worker_ref::defer_completion(), the completion, cancellation, and
    // resume lane of an operation the worker already admitted.
    template <typename fn_type>
        requires detail::move_only_function_target<void, fn_type>
    [[nodiscard]] bool defer_cleanup(fn_type&& fn) const {
        const event_loop snapshot = *this;
        return snapshot.defer_cleanup_task(move_only_function<void()>(std::forward<fn_type>(fn)));
    }

    // Starts one lazy task on this loop. Accepted: the loop owns the started
    // coroutine until completion and the result holds its structured
    // completion owner; abandoning that root_task never destroys a suspended
    // frame, and an abandoned failure is routed to the loop failure sink.
    // Rejected: a full admission queue (queue_full) or a stopping or retired
    // loop (worker_stopping) returns the unconsumed cold task. Admission is
    // reserved before the task moves, so a rejection never consumes it. Throws
    // only for allocation failure.
    template <typename t_type>
        requires detail::asio_task_result<t_type>
    [[nodiscard]] event_loop_start_result<t_type> try_start(task<t_type> task_value) const {
        using result_type = event_loop_start_result<t_type>;
        auto retirement_lease = acquire_root_lease();
        if (!retirement_lease) {
            return detail::worker_delivery_access::reject<result_type>(post_status::worker_stopping, std::move(task_value));
        }
        auto completion = std::make_shared<detail::root_task_state<t_type>>(worker(), state_);
        const auto bound_executor = executor();
        const auto status = worker().post_factory([&]() -> move_only_function<void()> {
            return [bound_executor, retirement_lease = std::move(retirement_lease),
                       launch = detail::root_task_launch_guard<t_type>(std::move(task_value), completion)]() mutable {
                try {
                    async_start(std::move(*launch.task_),
                        detail::root_task_completion_handler<t_type>(bound_executor, launch.completion_, std::move(retirement_lease)));
                    launch.release();
                } catch (...) {
                    auto failed_completion = std::move(launch.completion_);
                    launch.release();
                    failed_completion->completion().complete_failure(std::current_exception());
                }
            };
        });
        if (status != post_status::accepted) {
            return detail::worker_delivery_access::reject<result_type>(status, std::move(task_value));
        }
        return detail::worker_delivery_access::accept<result_type>(root_task<t_type>(std::move(completion)));
    }

    // The local strict form of try_start(): a rejection destroys the cold task
    // and throws event_loop_start_rejected carrying its status.
    template <typename t_type>
        requires detail::asio_task_result<t_type>
    [[nodiscard]] root_task<t_type> start(task<t_type> task_value) const {
        auto started = try_start(std::move(task_value));
        if (!started.accepted()) {
            throw event_loop_start_rejected(started.status());
        }
        return std::move(started).take_value();
    }

    // Stop cleanup is asynchronous and joined before runtime retirement. The
    // callable owner remains address-stable until its returned task and
    // completion handler have both finished. Resetting, replacing or destroying
    // the registration only unregisters cleanup that has not started.
    // Cancelling an unstarted callback moves and destroys its target outside
    // the listener lock; those operations may request loop stop.
    template <typename fn_type>
        requires std::invocable<fn_type&> &&
                 std::same_as<std::invoke_result_t<fn_type&>, task<void>> &&
                 std::move_constructible<std::decay_t<fn_type>>
    [[nodiscard]] event_loop_stop_registration on_stop(fn_type&& fn) const {
        const event_loop snapshot = *this;
        return snapshot.register_stop_callback(
            move_only_function<task<void>()>(std::forward<fn_type>(fn)));
    }

private:
    explicit event_loop(std::shared_ptr<detail::event_loop_state> state_value) noexcept;
    [[nodiscard]] event_loop_stop_registration register_stop_callback(
        move_only_function<task<void>()> callback) const;
    [[nodiscard]] std::shared_ptr<void> acquire_root_lease() const;
    [[nodiscard]] bool defer_cleanup_task(move_only_function<void()> task) const;

    std::shared_ptr<detail::event_loop_state> state_;
    friend class event_loop_pool;
    friend class event_loop_attachment;
    friend class worker_runtime;
};

struct event_loop_attachment_options final {
    std::size_t queue_capacity_{1024};
    // Borrowed pool for run_blocking() and client host lookups on this loop;
    // it must outlive the attachment. Null leaves the loop without one.
    blocking_pool* blocking_pool_{nullptr};
    // Owns the receiver for each reported failure (including abandoned root
    // tasks and failed stop callbacks). Empty uses report_unhandled_failure.
    // Invoked synchronously, outside runtime locks, on the reporting thread:
    // normally the context thread, but report_failure() or abandoning an
    // already-completed root may report from another thread. Calls may overlap
    // or reenter; the receiver must synchronize any shared mutable state.
    // Requests attachment stop before invocation. The receiver may destroy
    // the attachment or loop handles; its target stays alive through invocation
    // and is retained until the last loop-state owner releases it. Borrowed
    // captures must remain alive that long; do not capture an owning loop.
    // Receiver exceptions go to report_unhandled_failure, never escape run(),
    // and do not interrupt retirement or recursively invoke this receiver.
    move_only_function<void(std::exception_ptr)> failure_{};
};

// Binds a worker to an io_context the caller owns. The caller drives that
// context on exactly one thread, either directly with io_context::run() or
// with event_loop_attachment::run(), which also installs Ruvia's worker
// identity. The attachment never calls io_context::stop() and never restarts
// or joins a context it does not own.
//
// Retirement runs the same stop/finalize sequence as an event_loop_pool loop.
// stop(), or destroying the attachment, closes admission at once, from any
// thread, including while another thread is inside run(). Stop callbacks,
// root tasks, and deferred cleanup then drain, timers are cancelled, and the
// worker retires on the context's thread. Until then the attachment holds
// work on the context, so the caller keeps running it: a plain run() returns
// once retirement and the caller's own work are done. valid() turns false
// when retirement completes, for callers whose own work keeps run() busy.
//
// Destroying the io_context before retirement completes is valid only while
// no root task, stop callback, or deferred cleanup is outstanding; the
// context's shutdown then retires the worker synchronously. Destroying it
// with such work outstanding violates this contract: the shutdown reports a
// std::logic_error to the unhandled-failure diagnostic and terminates,
// because that work is owned by handlers that will never run.
//
// After retirement, event_loop handles become invalid; io_context() and
// executor() throw std::logic_error. The external io_context remains owned
// by its caller.
class event_loop_attachment final {
public:
    ~event_loop_attachment();

    event_loop_attachment(const event_loop_attachment&) = delete;
    event_loop_attachment& operator=(const event_loop_attachment&) = delete;
    // Move construction transfers one attachment without touching its context.
    // Move assignment would implicitly stop the target attachment, so it stays
    // deleted and ownership transfer remains explicit.
    event_loop_attachment(event_loop_attachment&& other) noexcept;
    event_loop_attachment& operator=(event_loop_attachment&& other) = delete;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] event_loop loop() const noexcept;
    // Runs the attached context on the current thread with Ruvia's worker
    // identity installed. A callback may destroy the attachment during run();
    // the caller-owned context must remain alive until this call returns.
    void run();
    void stop() noexcept;

private:
    explicit event_loop_attachment(std::shared_ptr<detail::event_loop_state> state_value) noexcept;

    std::shared_ptr<detail::event_loop_state> state_;
    friend event_loop_attachment attach_event_loop(asio::io_context&, event_loop_attachment_options);
};

// Attach a Ruvia worker to a caller-owned io_context. The caller retains
// ownership of the context's unrelated work and stop/restart policy; stop()
// retires only Ruvia-managed work and never cancels unrelated operations.
[[nodiscard]] event_loop_attachment attach_event_loop(
    asio::io_context& io_context, event_loop_attachment_options options = {});

struct event_loop_pool_options final {
    std::size_t loop_count_{0};
    std::size_t queue_capacity_{1024};
    // One borrowed pool shared by every loop for run_blocking() and client
    // host lookups; it must outlive the pool. Null leaves the loops without one.
    blocking_pool* blocking_pool_{nullptr};
};

class event_loop_pool final {
public:
    explicit event_loop_pool(event_loop_pool_options options = {});
    ~event_loop_pool();

    event_loop_pool(const event_loop_pool&) = delete;
    event_loop_pool& operator=(const event_loop_pool&) = delete;
    event_loop_pool(event_loop_pool&&) = delete;
    event_loop_pool& operator=(event_loop_pool&&) = delete;

    void start();
    void stop() noexcept;
    // Stops the pool and waits for every worker to finish its managed cleanup.
    // If join() happens before start(), it creates short-lived owner threads to
    // drain accepted work and run stop callbacks. Loop handles become invalid
    // once joined. Calling join() from a pool worker throws logic_error before
    // stopping the pool.
    void join();

    [[nodiscard]] std::size_t loop_count() const noexcept;
    [[nodiscard]] event_loop loop(std::size_t index) const;
    [[nodiscard]] event_loop next_loop() noexcept;
    [[nodiscard]] event_loop loop_for(std::uint64_t key) const noexcept;
    [[nodiscard]] event_loop loop_for(std::string_view key) const noexcept;

private:
    struct impl_type;
    std::shared_ptr<impl_type> impl_;
};

class worker_runtime;

struct worker_notification_options final {
    // Bounds the Windows native wake-pipe connection wait (at most one minute).
    // Synchronous setup system calls are not hard-real-time bounded.
    timeout_duration startup_timeout_{std::chrono::seconds(5)};
};

// coalesced means an existing pending notification covers this call;
// notifications are latched, not counted.
enum class worker_notification_status : std::uint8_t {
    notified,
    coalesced,
    closed,
};

enum class worker_notification_wait_status : std::uint8_t {
    notified,
    closed,
};

namespace detail {
class worker_notification_state;
}  // namespace detail

// A startup-created, address-stable, cross-thread latched wake channel for one
// worker runtime. The native wake pair is created at construction; the receiver
// Asio handle is adopted on the worker's first wait. The notification borrows
// its worker runtime or event loop, which must outlive it. Before destruction, every
// notify() call must have returned and no future call may begin. close()
// rejects new notifications but does not join producers. The owner worker must
// explicitly close the notification and join every suspended wait before its
// runtime stops. A cold, never-awaited instance can be destroyed directly.
class worker_notification final {
public:
    class wait_operation final {
    public:
        wait_operation(const wait_operation&) = delete;
        wait_operation& operator=(const wait_operation&) = delete;
        void operator&() const = delete;

        [[nodiscard]] bool await_ready();
        [[nodiscard]] bool await_suspend(std::coroutine_handle<> continuation);
        [[nodiscard]] worker_notification_wait_status await_resume();

    private:
        explicit wait_operation(worker_notification& owner_value) noexcept;

        worker_notification* owner_;
        friend class worker_notification;
    };

    explicit worker_notification(
        worker_runtime& runtime, worker_notification_options options = {});
    // Borrows the loop's worker; the loop must remain address-stable and alive.
    explicit worker_notification(
        const event_loop& loop, worker_notification_options options = {});
    worker_notification(event_loop&&, worker_notification_options = {}) = delete;
    worker_notification(const event_loop&&, worker_notification_options = {}) = delete;
    ~worker_notification();

    worker_notification(const worker_notification&) = delete;
    worker_notification& operator=(const worker_notification&) = delete;
    worker_notification(worker_notification&&) = delete;
    worker_notification& operator=(worker_notification&&) = delete;

    // Safe from any thread. The method updates its atomic latch and performs a
    // nonblocking native write; an existing latch or proven full channel coalesces.
    [[nodiscard]] worker_notification_status notify() noexcept;

    // Returns the one stable awaiter; waiting is worker-affine and permits one
    // outstanding wait. Constructing or discarding a cold task around wait()
    // does not register an operation. A notification racing with registration
    // may resume asynchronously.
    [[nodiscard]] wait_operation& wait() & noexcept;
    wait_operation& wait() && = delete;

    // Worker-affine and nonblocking. A pending wait completes as closed after
    // Asio delivers and drains its cancellation completion.
    void close();

private:
    using state_pointer = std::unique_ptr<detail::worker_notification_state>;

    explicit worker_notification(state_pointer state) noexcept;

    state_pointer state_;
    wait_operation wait_awaiter_;
};

namespace detail {

template <typename t_type>
inline constexpr bool is_owning_char_string = false;

template <typename traits_type, typename allocator_type>
inline constexpr bool is_owning_char_string<std::basic_string<char, traits_type, allocator_type>> = true;

template <typename t_type>
concept temporary_owning_char_string =
    is_owning_char_string<std::remove_cvref_t<t_type>> && !std::is_lvalue_reference_v<t_type&&>;

template <typename range_type>
concept temporary_owning_byte_range =
    !std::is_lvalue_reference_v<range_type&&> && std::ranges::contiguous_range<range_type> &&
    !std::ranges::borrowed_range<range_type> &&
    std::same_as<std::remove_cv_t<std::ranges::range_value_t<range_type>>, std::byte>;

}  // namespace detail

// HTTP and other octet streams are not Unicode text. These conversions are
// explicit views of the same bytes; they do not decode or validate charset.
[[nodiscard]] inline std::span<const std::byte> as_bytes(std::string_view text) noexcept {
    return std::as_bytes(std::span<const char>(text.data(), text.size()));
}
template <detail::temporary_owning_char_string string_type>
std::span<const std::byte> as_bytes(string_type&&) = delete;

[[nodiscard]] inline std::string_view as_chars(std::span<const std::byte> bytes_value) noexcept {
    return {reinterpret_cast<const char*>(bytes_value.data()), bytes_value.size()};
}
template <detail::temporary_owning_byte_range range_type>
std::string_view as_chars(range_type&&) = delete;

// UTF-8 text describing an error code, for every Ruvia message that embeds one.
// On Windows, message() of std::system_category() and asio::system_category()
// is FormatMessageA output in the process ANSI code page (for example GBK), which
// would corrupt UTF-8 responses, JSON and logs; codes of those categories are
// therefore formatted with FormatMessageW and converted to UTF-8, or described
// as "<category>:<value>" when the system has no message. Every other category,
// and every category on POSIX, uses message() unchanged.
[[nodiscard]] std::string error_code_text(const std::error_code& error);

// what() of a std::system_error with its error-code text produced by
// error_code_text. The supported standard libraries build what() as
// "<what_arg>: <code().message()>"; when what() does not end with that
// message, only error_code_text(code()) is returned.
[[nodiscard]] std::string system_error_text(const std::system_error& error);
// The last resort for a failure that has nowhere else to go: a detached task or
// a callback threw, no application sink was configured to receive it, and no
// caller will ever rethrow it. Writing one line to stderr is the only remaining
// way to keep it observable.
//
// Discarding it instead would make the fault undiagnosable, and terminating
// would turn one local failure into a whole-process one. Every layer routes its
// unowned failures here so a Ruvia process never fails silently, and so the
// diagnostic looks the same wherever it came from:
//
//     ruvia: <context> failed: <what>
//
// `context` names the failing unit ("web connection", "event loop stop callback").
// Writing is best effort -- stderr may be closed, full, or redirected -- but
// nothing beyond this point could report the failure either way.
//
// A lifecycle owner that rethrows one failure among several (application::run)
// reports only the ones it does not rethrow; the caller owns the rethrown one.
//
// Rate limited: a fault that fails every connection at once would otherwise
// bury the first, informative failure under its consequences, and a blocked
// stderr would slow the recovery it is describing. Excess reports are counted
// and the count travels with the next line that gets through, so the volume is
// bounded without the flood becoming invisible. An application that wants every
// failure should install its layer's sink instead of relying on this.
//
// `occurrences` > 1 reports that many equivalent failures (see
// equivalent_failures) as one line, so one root cause shared by several units
// is not repeated once per unit.
//
// A std::system_error is described by system_error_text,
// so the line stays UTF-8 on Windows.
void report_unhandled_failure(
    std::string_view context_value, std::exception_ptr exception, std::size_t occurrences = 1) noexcept;

// True for the same exception object, or for two std::exception objects of the
// same dynamic type with the same what(): the same fault seen by several units.
[[nodiscard]] bool equivalent_failures(const std::exception_ptr& left, const std::exception_ptr& right) noexcept;

// Parse a complete textual IP address, rejecting embedded NUL bytes.
[[nodiscard]] std::optional<asio::ip::address> parse_ip_address(std::string_view text);
// Close a socket and cancel any outstanding I/O.
inline void close_socket(asio::ip::tcp::socket& socket) noexcept {
    std::error_code ignored;
    socket.cancel(ignored);
    socket.shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
    socket.close(ignored);
}

}  // namespace ruvia
