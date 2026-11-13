#pragma once

#include <algorithm>
#include <chrono>
#include <concepts>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include "ruvia/core/failure_report.h"
#include "ruvia/core/operation_deadline.h"
#include "ruvia/core/operation_options.h"
#include "ruvia/core/pool_lease_scheduler.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/worker_cancellation.h"
#include "ruvia/core/worker_ref.h"
#include "ruvia/core/worker_timer.h"
#include "ruvia/core/worker_wait.h"

namespace ruvia::detail {

// A failed claim leaves the lane untouched; the caller supplies its domain error.
class operation_lane_lease final {
public:
    explicit operation_lane_lease(bool& active) noexcept
        : active_(active ? nullptr : &active) {
        if (active_ != nullptr) {
            *active_ = true;
        }
    }

    operation_lane_lease(const operation_lane_lease&) = delete;
    operation_lane_lease& operator=(const operation_lane_lease&) = delete;
    operation_lane_lease(operation_lane_lease&& other) noexcept
        : active_(std::exchange(other.active_, nullptr)) {}
    operation_lane_lease& operator=(operation_lane_lease&&) = delete;

    ~operation_lane_lease() noexcept {
        if (active_ != nullptr) {
            *active_ = false;
        }
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return active_ != nullptr;
    }

private:
    bool* active_;
};

[[nodiscard]] inline operation_lane_lease claim_operation_lane(bool& active, const char* in_progress_message) {
    operation_lane_lease lease(active);
    if (!lease) {
        throw std::logic_error(in_progress_message);
    }
    return lease;
}

}  // namespace ruvia::detail

namespace ruvia::detail {

struct request_io_metrics final {
    std::uint64_t db_count_{0};
    std::uint64_t db_duration_micros_{0};
    std::uint64_t redis_count_{0};
    std::uint64_t redis_duration_micros_{0};
};

enum class request_io_kind : std::uint8_t {
    db = 0,
    redis = 1,
};

class scoped_request_io_timer final {
public:
    // `metrics` is the request's counter block, or null outside a request
    // (worker callbacks, standalone clients); null disables timing.
    scoped_request_io_timer(request_io_metrics* metrics, request_io_kind kind) noexcept
        : metrics_(metrics),
          kind_(kind),
          started_at_(metrics_ != nullptr ? std::chrono::steady_clock::now()
                                          : std::chrono::steady_clock::time_point{}) {}

    ~scoped_request_io_timer() {
        if (metrics_ == nullptr) {
            return;
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started_at_)
                                 .count();
        const auto micros = elapsed < 0 ? std::uint64_t{0} : static_cast<std::uint64_t>(elapsed);
        if (kind_ == request_io_kind::db) {
            ++metrics_->db_count_;
            metrics_->db_duration_micros_ += micros;
        } else {
            ++metrics_->redis_count_;
            metrics_->redis_duration_micros_ += micros;
        }
    }

    scoped_request_io_timer(const scoped_request_io_timer&) = delete;
    scoped_request_io_timer& operator=(const scoped_request_io_timer&) = delete;

private:
    request_io_metrics* metrics_{nullptr};
    request_io_kind kind_{request_io_kind::db};
    std::chrono::steady_clock::time_point started_at_{};
};

}  // namespace ruvia::detail

namespace ruvia::detail {

inline constexpr std::string_view default_capability_alias = "default";

inline void validate_capability_alias(std::string_view alias, const char* empty_message) {
    if (alias.empty()) {
        throw std::invalid_argument(empty_message);
    }
}

// Every named worker capability has the same startup representation: an alias
// plus validated, normalized configuration storage.
template <typename storage_type>
struct named_capability_definition final {
    using config_storage_type = storage_type;

    std::string alias_;
    storage_type config_;
};

// Replaces the definition registered under replacement.alias_ in place, so
// existing aliases preserve registration order, or appends a new alias.
template <typename definition_type>
void upsert_named_capability_definition(
    std::vector<definition_type>& definitions, definition_type&& replacement) {
    const std::string_view alias = replacement.alias_;
    for (auto& definition : definitions) {
        if (std::string_view(definition.alias_) == alias) {
            std::destroy_at(&definition);
            std::construct_at(&definition, std::move(replacement));
            return;
        }
    }
    definitions.push_back(std::move(replacement));
}

// Validates and normalizes the replacement before mutating retained application
// state; the new definition takes one owned copy of the name and configuration.
template <typename definition_type, typename source_config_type>
void upsert_named_capability_definition(std::vector<definition_type>& definitions,
    std::string_view alias, const source_config_type& source_config, const char* empty_message) {
    validate_capability_alias(alias, empty_message);
    upsert_named_capability_definition(definitions,
        definition_type{std::string(alias), typename definition_type::config_storage_type(source_config)});
}

// Validates the complete startup alias set without allocating. Capability
// owners call this before constructing pools, so an invalid later definition
// cannot leave a partially built worker capability graph behind.
template <typename entries_type>
void validate_capability_aliases(
    const entries_type& entries, const char* empty_message, const char* duplicate_message) {
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const std::string_view alias = entries[index].alias_;
        validate_capability_alias(alias, empty_message);
        for (std::size_t previous = 0; previous < index; ++previous) {
            if (std::string_view(entries[previous].alias_) == alias) {
                throw std::invalid_argument(duplicate_message);
            }
        }
    }
}

// Immutable startup-built index shared by worker-local named capabilities.
// It owns one copy of each alias and binds it to the source registration
// position; source definitions need not outlive build(). Request-time lookup
// performs one allocation-free binary search.
class named_capability_index final {
public:
    named_capability_index() = default;
    void build(std::initializer_list<std::string_view> aliases) {
        build_aliases(aliases, [](std::string_view alias) noexcept { return alias; });
    }

    template <typename entries_type>
    void build(const entries_type& entries) {
        build_aliases(
            entries, [](const auto& entry_value) noexcept -> std::string_view { return entry_value.alias_; });
    }

    [[nodiscard]] std::optional<std::size_t> find(std::string_view alias) const noexcept {
        const auto match =
            std::ranges::lower_bound(aliases_, alias, {}, &named_capability_index::alias_view);
        if (match == aliases_.end() || match->alias_ != alias) {
            return std::nullopt;
        }
        return match->index_;
    }

    [[nodiscard]] std::optional<std::size_t> default_index() const noexcept {
        return default_index_;
    }

private:
    struct indexed_alias_type final {
        std::string alias_;
        std::size_t index_;
    };

    [[nodiscard]] static std::string_view alias_view(const indexed_alias_type& entry_value) noexcept {
        return entry_value.alias_;
    }

    template <typename entries_type, typename alias_projection_type>
    void build_aliases(const entries_type& entries, alias_projection_type alias_projection) {
        std::vector<indexed_alias_type> aliases;
        aliases.reserve(entries.size());
        std::optional<std::size_t> default_index;
        std::size_t index = 0;
        for (const auto& entry : entries) {
            const std::string_view alias = alias_projection(entry);
            aliases.push_back(indexed_alias_type{std::string(alias), index});
            if (alias == default_capability_alias) {
                default_index = index;
            }
            ++index;
        }
        std::ranges::sort(aliases, {}, &named_capability_index::alias_view);
        aliases_.swap(aliases);
        default_index_ = default_index;
    }

    std::vector<indexed_alias_type> aliases_;
    std::optional<std::size_t> default_index_;
};

}  // namespace ruvia::detail

namespace ruvia::detail {

// Receipts cover execution AND destruction of posted controls. Waiting never
// cancels early: borrowed command state cannot retire before every receipt.
class runtime_control_completion final {
    struct state final {
        explicit state(worker_ref caller)
            : caller_(std::move(caller)) {}
        ~state() {
            if (failure_ && !observed_) {
                report_unhandled_failure("runtime control", failure_);
            }
        }
        worker_ref caller_;
        std::mutex mutex_;
        std::exception_ptr failure_;
        std::coroutine_handle<> continuation_{};
        std::size_t completed_{};
        bool released_{};
        bool observed_{};
    };

public:
    template <typename function_type>
    class command;
    class receipt final {
    public:
        void complete(std::exception_ptr failure = {}) const noexcept {
            std::lock_guard lock(state_->mutex_);
            ++state_->completed_;
            if (!state_->failure_) {
                state_->failure_ = std::move(failure);
            }
        }
        template <typename function_type>
        [[nodiscard]] command<std::decay_t<function_type>> bind(function_type function) && {
            return command<std::decay_t<function_type>>(std::move(*this), std::move(function));
        }

    private:
        receipt(std::shared_ptr<state> state_value, std::shared_ptr<void> lifetime) noexcept
            : state_(std::move(state_value)),
              lifetime_(std::move(lifetime)) {}
        std::shared_ptr<state> state_;
        std::shared_ptr<void> lifetime_;
        friend class runtime_control_completion;
    };

    // Capture lists do not specify closure member declaration order. Keep the
    // receipt in a named first member so command payload destruction completes
    // before its final receipt can resume another worker.
    template <typename function_type>
    class command final {
    public:
        command(receipt completion, function_type function)
            : completion_(std::move(completion)),
              function_(std::move(function)) {}
        command(const command&) = delete;
        command& operator=(const command&) = delete;
        command(command&&) = default;
        command& operator=(command&&) = delete;
        template <typename... args_type>
            requires std::invocable<function_type&, const receipt&, args_type...>
        decltype(auto) operator()(args_type&&... args) {
            return std::invoke(function_, std::as_const(completion_), std::forward<args_type>(args)...);
        }

    private:
        receipt completion_;
        function_type function_;
    };

    class wait_operation final {
    public:
        wait_operation(const wait_operation&) = delete;
        wait_operation& operator=(const wait_operation&) = delete;
        void operator&() const = delete;
        ~wait_operation() {
            if (started_ && !resumed_) {
                std::terminate();
            }
        }
        bool await_ready() {
            if (!state_->caller_.is_current()) {
                throw std::logic_error("runtime control completion must be awaited on its caller worker");
            }
            std::lock_guard lock(state_->mutex_);
            started_ = true;
            return state_->released_;
        }
        bool await_suspend(std::coroutine_handle<> continuation) {
            std::lock_guard lock(state_->mutex_);
            if (state_->released_) {
                return false;
            }
            if (state_->continuation_) {
                std::terminate();
            }
            state_->continuation_ = continuation;
            return true;
        }
        void await_resume() noexcept {
            resumed_ = true;
        }

    private:
        explicit wait_operation(std::shared_ptr<state> value) noexcept
            : state_(std::move(value)) {}
        std::shared_ptr<state> state_;
        bool started_{};
        bool resumed_{};
        friend class runtime_control_completion;
    };
    explicit runtime_control_completion(worker_ref caller)
        : state_(std::make_shared<state>(std::move(caller))),
          lifetime_(state_.get(), [value = state_](void*) noexcept {
              std::coroutine_handle<> continuation;
              {
                  std::lock_guard lock(value->mutex_);
                  value->released_ = true;
                  continuation = std::exchange(value->continuation_, {});
              }
              if (continuation) {
                  value->caller_.defer_resume(continuation);
              }
          }) {}
    runtime_control_completion(const runtime_control_completion&) = delete;
    runtime_control_completion& operator=(const runtime_control_completion&) = delete;
    [[nodiscard]] receipt make_receipt() const {
        if (!lifetime_) {
            throw std::logic_error("runtime control completion is already waiting");
        }
        return receipt(state_, lifetime_);
    }
    template <typename function_type>
    [[nodiscard]] command<std::decay_t<function_type>> make_command(function_type function) const {
        return make_receipt().bind(std::move(function));
    }
    [[nodiscard]] wait_operation wait() & {
        if (!state_->caller_.is_current()) {
            throw std::logic_error("runtime control must wait on its caller worker");
        }
        if (waiting_) {
            throw std::logic_error("runtime control completion can only wait once");
        }
        waiting_ = true;
        lifetime_.reset();
        return wait_operation(state_);
    }
    wait_operation wait() && = delete;
    [[nodiscard]] std::size_t completed() const noexcept {
        std::lock_guard lock(state_->mutex_);
        return state_->completed_;
    }
    [[nodiscard]] std::exception_ptr failure() const noexcept {
        std::lock_guard lock(state_->mutex_);
        state_->observed_ = true;
        return state_->failure_;
    }

private:
    std::shared_ptr<state> state_;
    std::shared_ptr<void> lifetime_;
    bool waiting_{};
};

}  // namespace ruvia::detail

namespace ruvia::detail {

// Serializes admitted controls without holding a mutex across suspension.
// The owning state must outlive every acquire and granted lease.
class runtime_control_gate final {
public:
    class acquire_operation;
    class lease final {
    public:
        lease(const lease&) = delete;
        lease& operator=(const lease&) = delete;
        lease(lease&& other) noexcept
            : gate_(std::exchange(other.gate_, nullptr)) {}
        lease& operator=(lease&&) = delete;
        ~lease() {
            if (gate_) {
                gate_->release();
            }
        }

    private:
        explicit lease(runtime_control_gate& gate) noexcept
            : gate_(&gate) {}
        runtime_control_gate* gate_;
        friend class acquire_operation;
    };

    class acquire_operation final {
    public:
        acquire_operation(const acquire_operation&) = delete;
        acquire_operation& operator=(const acquire_operation&) = delete;
        void operator&() const = delete;
        ~acquire_operation() {
            if (started_ && !resumed_) {
                std::terminate();
            }
        }
        bool await_ready() {
            if (!caller_.is_current()) {
                throw std::logic_error("runtime control must be awaited on its caller worker");
            }
            std::lock_guard lock(gate_.mutex_);
            started_ = true;
            if (gate_.active_) {
                return false;
            }
            gate_.active_ = true;
            return true;
        }
        bool await_suspend(std::coroutine_handle<> continuation) {
            std::lock_guard lock(gate_.mutex_);
            if (!gate_.active_) {
                gate_.active_ = true;
                return false;
            }
            continuation_ = continuation;
            if (gate_.tail_) {
                gate_.tail_->next_ = this;
            } else {
                gate_.head_ = this;
            }
            gate_.tail_ = this;
            return true;
        }
        lease await_resume() noexcept {
            resumed_ = true;
            return lease(gate_);
        }

    private:
        acquire_operation(runtime_control_gate& gate, worker_ref caller) noexcept
            : gate_(gate),
              caller_(std::move(caller)) {}
        runtime_control_gate& gate_;
        worker_ref caller_;
        acquire_operation* next_{};
        std::coroutine_handle<> continuation_{};
        bool started_{};
        bool resumed_{};
        friend class runtime_control_gate;
    };

    runtime_control_gate() = default;
    runtime_control_gate(const runtime_control_gate&) = delete;
    runtime_control_gate& operator=(const runtime_control_gate&) = delete;
    ~runtime_control_gate() {
        if (active_ || head_) {
            std::terminate();
        }
    }
    [[nodiscard]] acquire_operation acquire(worker_ref caller) & noexcept {
        return acquire_operation(*this, std::move(caller));
    }
    acquire_operation acquire(worker_ref) && = delete;

private:
    void release() noexcept {
        worker_ref caller;
        std::coroutine_handle<> continuation;
        {
            std::lock_guard lock(mutex_);
            if (!head_) {
                active_ = false;
                return;
            }
            auto* next = head_;
            head_ = next->next_;
            if (!head_) {
                tail_ = nullptr;
            }
            next->next_ = nullptr;
            caller = next->caller_;
            continuation = std::exchange(next->continuation_, {});
        }
        caller.defer_resume(continuation);
    }
    std::mutex mutex_;
    acquire_operation* head_{};
    acquire_operation* tail_{};
    bool active_{};
};

}  // namespace ruvia::detail

// The connection-slot bookkeeping every pooled client shares: lease scheduling,
// stop-token cancellation addressed by operation id, and one deadline timer per
// slot. Only the error type and what an expiry or cancellation does to the
// driver's connection differ, so those are the policy.

namespace ruvia::detail {

enum class pool_slot_abort_reason : std::uint8_t {
    none,
    cancelled,
    closing,
};

// policy_type supplies:
//   using owner_type = ...;          // the driver pool that owns the connections
//   using deadline_kind_type = ...;  // scoped enum naming what a deadline guards
//   [[noreturn]] static void throw_acquire_failure(const owner_type&, worker_wait_status);
//   [[noreturn]] static void throw_aborted(const owner_type&, pool_slot_abort_reason);
//   static void expire_slot(owner_type&, std::size_t slot, deadline_kind_type) noexcept;
//   static void cancel_slot(owner_type&, std::size_t slot) noexcept;
//
// The pool is worker-affine and address-stable: slot state lives in one array
// sized at construction, and pending timers and cancellation registrations
// borrow it by address.
template <typename policy_type>
class slot_pool final {
public:
    using owner_type = typename policy_type::owner_type;
    using deadline_kind_type = typename policy_type::deadline_kind_type;
    using cancellation_target_type = worker_cancellation_target<slot_pool>;

    slot_pool(owner_type& owner_value, std::size_t slot_count, const worker_ref& worker_value)
        : owner_(owner_value),
          worker_(worker_value),
          slots_(slot_count),
          scheduler_(slot_count, worker_),
          cancellation_target_(make_worker_cancellation_target(*this, worker_)) {}

    slot_pool(owner_type&, std::size_t, worker_ref&&) = delete;

    ~slot_pool() {
        cancellation_target_->detach(*this);
    }

    slot_pool(const slot_pool&) = delete;
    slot_pool& operator=(const slot_pool&) = delete;
    slot_pool(slot_pool&&) = delete;
    slot_pool& operator=(slot_pool&&) = delete;

    [[nodiscard]] const worker_ref& worker() const noexcept {
        return worker_;
    }

    class acquire_awaiter final {
    public:
        acquire_awaiter(owner_type& owner, pool_lease_acquire acquire) noexcept
            : owner_(owner),
              acquire_(std::move(acquire)) {}
        acquire_awaiter(const acquire_awaiter&) = delete;
        acquire_awaiter& operator=(const acquire_awaiter&) = delete;
        acquire_awaiter(acquire_awaiter&&) noexcept = default;
        acquire_awaiter& operator=(acquire_awaiter&&) = delete;
        void operator&() const = delete;

        [[nodiscard]] bool await_ready() {
            return acquire_.await_ready();
        }
        [[nodiscard]] bool await_suspend(std::coroutine_handle<> continuation) {
            return acquire_.await_suspend(continuation);
        }
        [[nodiscard]] std::size_t await_resume() {
            const auto result_value = acquire_.await_resume();
            if (result_value.has_value()) {
                return result_value.value();
            }
            policy_type::throw_acquire_failure(owner_, result_value.status());
        }

    private:
        owner_type& owner_;
        pool_lease_acquire acquire_;
    };

    // The common free-slot path completes directly in the caller's frame.
    // Saturated pools use the same scheduler, timeout and cancellation path.
    [[nodiscard]] acquire_awaiter acquire(const operation_deadline& timeout,
        std::optional<timeout_duration> acquire_timeout, stop_token stop_token_value) {
        return acquire_awaiter(owner_, scheduler_.acquire({.timeout_ = timeout.constrained_by(acquire_timeout).timeout(),
                                           .stop_token_ = std::move(stop_token_value)}));
    }

    // Leases a free slot without waiting; nullopt while every slot is leased
    // or the pool is closing. A free slot is exactly what acquire() leases
    // without suspending.
    [[nodiscard]] std::optional<std::size_t> try_acquire() {
        auto acquire = scheduler_.acquire();
        if (!acquire.await_ready()) {
            return std::nullopt;
        }
        const auto result_value = acquire.await_resume();
        if (!result_value.has_value()) {
            return std::nullopt;
        }
        return result_value.value();
    }

    // A release that names no live lease is a caller bug and cannot be
    // reported through a noexcept path.
    void release(std::size_t slot) noexcept {
        scheduler_.release(slot);
    }

    // Refuses new leases and marks every slot without an abort reason as
    // closing; on true the owner must abort every connection, since a later
    // stop no longer reaches cancel_slot(). Returns false when the pool was
    // already closed; the owner then has nothing left to tear down.
    [[nodiscard]] bool close() noexcept {
        if (!scheduler_.close()) {
            return false;
        }
        for (auto& slot : slots_) {
            if (slot.abort_reason_ == pool_slot_abort_reason::none) {
                slot.abort_reason_ = pool_slot_abort_reason::closing;
            }
        }
        return true;
    }

    [[nodiscard]] bool closing() const noexcept {
        return scheduler_.closing();
    }

    // Starts an operation on a leased slot: clears a cancellation its previous
    // operation left, while closing stays permanent.
    void reset_abort(std::size_t slot) noexcept {
        slots_[slot].abort_reason_ = closing() ? pool_slot_abort_reason::closing : pool_slot_abort_reason::none;
    }

    // Releases a lease on scope exit, however the operation between acquire
    // and release ends.
    class lease_guard final {
    public:
        lease_guard(slot_pool& pool, std::size_t slot) noexcept
            : pool_(pool),
              slot_(slot) {}
        lease_guard(const lease_guard&) = delete;
        lease_guard& operator=(const lease_guard&) = delete;
        ~lease_guard() {
            pool_.release(slot_);
        }

    private:
        slot_pool& pool_;
        std::size_t slot_;
    };

    // Binds the operation's stop token to one slot for the duration of the
    // operation. A stop delivered on (or posted to) the worker reaches
    // cancel_operation_by_id(), which marks the slot cancelled and lets the
    // policy abort the driver connection. A cancellation belongs to the
    // operation it ended; closing is permanent, so an operation still holding
    // a lease after close() (an open stream or transaction) observes it, even
    // when its stop arrives later.
    class cancellation_guard final {
    public:
        cancellation_guard(slot_pool& pool, std::size_t slot, const stop_token& stop_token_value)
            : registration_(pool.cancellation_target_, pool.slots_[slot].cancellation_id_) {
            pool.reset_abort(slot);
            registration_.arm(stop_token_value);
        }
        cancellation_guard(const cancellation_guard&) = delete;
        cancellation_guard& operator=(const cancellation_guard&) = delete;
        ~cancellation_guard() {
            finish();
        }

        void finish() noexcept {
            registration_.reset();
        }

        // The operation's id; cancel_operation_by_id() with it acts as a stop.
        [[nodiscard]] std::uint64_t id() const noexcept {
            return registration_.id();
        }

    private:
        worker_cancellation_registration<cancellation_target_type> registration_;
    };

    // worker_cancellation_target hook; runs on the worker. A closing slot
    // keeps reporting closing: its owner aborts every connection right after
    // close(), so a later stop has nothing left to cancel.
    void cancel_operation_by_id(std::uint64_t cancellation_id) noexcept {
        if (cancellation_id == 0) {
            return;
        }
        for (std::size_t index = 0; index < slots_.size(); ++index) {
            auto& slot = slots_[index];
            if (slot.cancellation_id_ != cancellation_id) {
                continue;
            }
            slot.cancellation_id_ = 0;
            if (slot.abort_reason_ != pool_slot_abort_reason::closing) {
                slot.abort_reason_ = pool_slot_abort_reason::cancelled;
                policy_type::cancel_slot(owner_, index);
            }
            return;
        }
    }

    [[nodiscard]] bool aborted(std::size_t slot) const noexcept {
        return slots_[slot].abort_reason_ != pool_slot_abort_reason::none;
    }

    void throw_if_aborted(std::size_t slot) const {
        const auto reason = slots_[slot].abort_reason_;
        if (reason != pool_slot_abort_reason::none) {
            policy_type::throw_aborted(owner_, reason);
        }
    }

    // Replaces the slot's absolute deadline. An absent deadline leaves the
    // slot unarmed; an expired one returns false so the caller reports its own
    // timeout. A scheduling failure rolls the slot back before propagating.
    [[nodiscard]] bool arm_deadline(
        std::size_t slot, std::optional<std::chrono::steady_clock::time_point> deadline, deadline_kind_type kind) {
        auto& state = slots_[slot];
        (void)clear_deadline(slot);
        if (!deadline.has_value()) {
            return true;
        }
        if (*deadline <= std::chrono::steady_clock::now()) {
            return false;
        }
        const auto deadline_value = *deadline;
        state.deadline_.arm(deadline_value, kind);
        try {
            (void)worker_.schedule_timer(state.deadline_timer_, deadline_value,
                [this, slot](worker_timer_outcome outcome) noexcept {
                    if (outcome != worker_timer_outcome::expired) {
                        return;
                    }
                    const auto expired = slots_[slot].deadline_.expire(std::chrono::steady_clock::now());
                    if (expired.has_value()) {
                        policy_type::expire_slot(owner_, slot, *expired);
                    }
                });
        } catch (...) {
            state.deadline_timer_.cancel();
            state.deadline_.reset();
            throw;
        }
        return true;
    }

    // Disarms the slot's deadline; true when it had already expired.
    [[nodiscard]] bool clear_deadline(std::size_t slot) noexcept {
        auto& state = slots_[slot];
        state.deadline_timer_.cancel();
        return state.deadline_.clear();
    }

    // Expires an armed deadline whose time has passed without waiting for the
    // timer; true when the deadline is (now) expired.
    [[nodiscard]] bool poll_deadline(std::size_t slot) noexcept {
        auto& deadline = slots_[slot].deadline_;
        return deadline.expire(std::chrono::steady_clock::now()).has_value() || deadline.expired();
    }

    [[nodiscard]] const deadline_kind_type* deadline_kind(std::size_t slot) const noexcept {
        return slots_[slot].deadline_.kind();
    }

private:
    struct slot_state final {
        std::uint64_t cancellation_id_{0};
        pool_slot_abort_reason abort_reason_{pool_slot_abort_reason::none};
        deadline_lifecycle<deadline_kind_type> deadline_;
        worker_timer_registration deadline_timer_;
    };

    // Fixed-size, never relocated: timer registrations are neither movable
    // nor copyable, and pending timers address their slot by index.
    class slot_array final {
    public:
        explicit slot_array(std::size_t slot_count)
            : data_(std::make_unique<slot_state[]>(slot_count)),
              size_(slot_count) {}
        slot_array(const slot_array&) = delete;
        slot_array& operator=(const slot_array&) = delete;

        [[nodiscard]] std::size_t size() const noexcept {
            return size_;
        }
        [[nodiscard]] slot_state& operator[](std::size_t index) noexcept {
            return data_[index];
        }
        [[nodiscard]] const slot_state& operator[](std::size_t index) const noexcept {
            return data_[index];
        }
        [[nodiscard]] slot_state* begin() noexcept {
            return data_.get();
        }
        [[nodiscard]] slot_state* end() noexcept {
            return data_.get() + size_;
        }

    private:
        std::unique_ptr<slot_state[]> data_;
        std::size_t size_;
    };

    owner_type& owner_;
    const worker_ref& worker_;
    slot_array slots_;
    pool_lease_scheduler scheduler_;
    std::shared_ptr<cancellation_target_type> cancellation_target_;
};

}  // namespace ruvia::detail

namespace ruvia::detail {

class operation_interrupt;

// Routes stop requests to the running operations of one I/O owner on its
// worker. The owner keeps it at a stable address until those operations have
// completed. The first stoppable operation creates the shared target; it is
// detached on destruction, so a stop delivered later reaches no owner.
class operation_interrupt_source final {
public:
    operation_interrupt_source() noexcept = default;
    operation_interrupt_source(const operation_interrupt_source&) = delete;
    operation_interrupt_source& operator=(const operation_interrupt_source&) = delete;
    ~operation_interrupt_source() {
        if (target_) {
            target_->detach(*this);
        }
    }

    void cancel_operation_by_id(std::uint64_t id) noexcept;

private:
    friend class operation_interrupt;

    std::shared_ptr<worker_cancellation_target<operation_interrupt_source>> target_;
    operation_interrupt* running_{};
};

enum class operation_interrupt_cause : std::uint8_t { cancelled,
    timed_out };

// The deadline and stop token of one running operation. Both are delivered on
// the operation's worker, where the first records its cause and calls
// `interrupt(target)` once: a read wakes its wait and keeps buffered input, an
// output operation aborts its stream. A stop requested before construction
// throws operation_canceled before anything is armed, so the operation has no
// effect. Construct it before the operation's first wait and keep it until
// that operation completes.
class operation_interrupt final {
public:
    using interrupt_type = void (*)(void*) noexcept;

    operation_interrupt(operation_interrupt_source& source, const worker_ref& worker_value,
        const operation_options& options, void* target, interrupt_type interrupt, const char* operation_name)
        : source_(source),
          target_(target),
          interrupt_(interrupt),
          operation_name_(operation_name) {
        if (options.stop_token_.stop_requested()) {
            throw_cause(operation_interrupt_cause::cancelled, operation_name_);
        }
        if (options.stop_token_.stoppable() && !source_.target_) {
            source_.target_ = make_worker_cancellation_target(source_, worker_value);
        }
        if (options.timeout_.has_value()) {
            (void)worker_value.schedule_timer(timer_, options.timeout_->deadline_from(std::chrono::steady_clock::now()),
                [this](worker_timer_outcome outcome) noexcept {
                    if (outcome == worker_timer_outcome::expired) {
                        fire(operation_interrupt_cause::timed_out);
                    }
                });
        }
        // Linked before arming: a stop requested meanwhile is delivered inline.
        next_ = source_.running_;
        source_.running_ = this;
        if (options.stop_token_.stoppable()) {
            try {
                cancellation_.emplace(source_.target_, id_);
                cancellation_->arm(options.stop_token_);
            } catch (...) {
                cancellation_.reset();
                unlink();
                throw;
            }
        }
    }

    operation_interrupt(const operation_interrupt&) = delete;
    operation_interrupt& operator=(const operation_interrupt&) = delete;

    ~operation_interrupt() {
        // Joins a stop callback before this operation leaves the source.
        cancellation_.reset();
        unlink();
    }

    [[nodiscard]] bool interrupted() const noexcept {
        return cause_.has_value();
    }

    [[nodiscard]] std::optional<operation_interrupt_cause> cause() const noexcept {
        return cause_;
    }

    // Throws std::system_error (timed_out or operation_canceled) once interrupted.
    void throw_if_interrupted() const {
        if (cause_.has_value()) {
            throw_cause(*cause_, operation_name_);
        }
    }

    [[noreturn]] static void throw_cause(operation_interrupt_cause cause, const char* operation_name) {
        throw std::system_error(std::make_error_code(cause == operation_interrupt_cause::timed_out
                                                         ? std::errc::timed_out
                                                         : std::errc::operation_canceled),
            operation_name);
    }

private:
    friend class operation_interrupt_source;

    void unlink() noexcept {
        for (auto** link = &source_.running_; *link != nullptr; link = &(*link)->next_) {
            if (*link == this) {
                *link = next_;
                return;
            }
        }
    }

    void fire(operation_interrupt_cause cause) noexcept {
        if (cause_.has_value()) {
            return;
        }
        cause_ = cause;
        interrupt_(target_);
    }

    operation_interrupt_source& source_;
    void* target_;
    interrupt_type interrupt_;
    const char* operation_name_;
    std::optional<operation_interrupt_cause> cause_;
    operation_interrupt* next_{};
    std::uint64_t id_{};
    worker_timer_registration timer_;
    // Declared last: its reset joins a pending stop callback first.
    std::optional<worker_cancellation_registration<worker_cancellation_target<operation_interrupt_source>>> cancellation_;
};

inline void operation_interrupt_source::cancel_operation_by_id(std::uint64_t id) noexcept {
    for (auto* running = running_; running != nullptr; running = running->next_) {
        if (id != 0 && running->id_ == id) {
            running->fire(operation_interrupt_cause::cancelled);
            return;
        }
    }
}

// One operation made of several interruptible steps: each step runs with the
// time left and the same stop token, and a step's interruption is recognized
// as this operation's own.
class operation_steps final {
public:
    explicit operation_steps(operation_options operation) noexcept
        : deadline_(operation.timeout_),
          stop_(std::move(operation.stop_token_)) {}

    // The time left and the same stop token for the next step; std::nullopt
    // once the deadline has passed.
    [[nodiscard]] std::optional<operation_options> try_next() const {
        if (deadline_.expired()) {
            return std::nullopt;
        }
        return operation_options{.timeout_ = deadline_.timeout(), .stop_token_ = stop_};
    }

    // Throws std::system_error (timed_out) once the deadline has passed.
    [[nodiscard]] operation_options next(const char* operation_name) const {
        auto step = try_next();
        if (!step.has_value()) {
            operation_interrupt::throw_cause(operation_interrupt_cause::timed_out, operation_name);
        }
        return std::move(*step);
    }

    // Whether this operation's own deadline or stop token accounts for `cause`.
    [[nodiscard]] bool interrupted_by(operation_interrupt_cause cause) const noexcept {
        return cause == operation_interrupt_cause::timed_out ? deadline_.expired() : stop_.stop_requested();
    }

    // Whether `error` is a step interrupted by this operation's deadline or stop token.
    [[nodiscard]] bool interrupted(const std::system_error& error) const noexcept {
        return (error.code() == std::errc::timed_out && interrupted_by(operation_interrupt_cause::timed_out)) ||
               (error.code() == std::errc::operation_canceled && interrupted_by(operation_interrupt_cause::cancelled));
    }

private:
    operation_deadline deadline_;
    stop_token stop_;
};

}  // namespace ruvia::detail
