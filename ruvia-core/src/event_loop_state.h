#pragma once

#include <atomic>
#include <cstddef>
#include <exception>
#include <memory>
#include <mutex>
#include <utility>
#include <variant>

#include <asio/io_context.hpp>

#include "move_only_function.h"
#include "worker_ref.h"
#include "worker_runtime.h"
#include "worker_runtime_context.h"

namespace ruvia::detail {

class external_context_attachment_service;
struct event_loop_state;

// Retirement gate of one loop: root tasks, stop callbacks, and cleanup each
// hold a pending count; finalize runs once the gate is sealed and drained.
class event_loop_retirement final {
public:
    event_loop_retirement() = default;

    // At most once. The callable moves in after the slot is reserved and is
    // claimed here when the gate sealed and drained in between.
    [[nodiscard]] bool begin_stop(move_only_function<void()> finalize) noexcept {
        {
            const std::lock_guard lock(mutex_);
            root_admission_closed_ = true;
            if (sealed_ || finalized_ || finalize_slot_ != finalize_slot_type::empty) {
                return false;
            }
            finalize_slot_ = finalize_slot_type::installing;
        }
        // The reservation keeps every other thread off finalize_ until it is
        // published, so the callable's move runs without the gate mutex.
        finalize_ = std::move(finalize);
        bool abandoned = false;
        bool claimed = false;
        {
            const std::lock_guard lock(mutex_);
            finalize_slot_ = finalize_slot_type::installed;
            abandoned = finalized_;
            claimed = !abandoned && claim_finalize_locked();
        }
        if (abandoned) {
            // close_admission() ran while the slot was reserved.
            finalize_ = nullptr;
        } else if (claimed) {
            run_claimed_finalize();
        }
        return true;
    }

    [[nodiscard]] bool try_begin() noexcept {
        const std::lock_guard lock(mutex_);
        if (sealed_ || finalized_) {
            return false;
        }
        ++pending_;
        return true;
    }

    [[nodiscard]] bool try_begin_cleanup() noexcept {
        const std::lock_guard lock(mutex_);
        if (finalized_) {
            return false;
        }
        ++pending_;
        return true;
    }

    void begin() noexcept {
        if (!try_begin()) {
            std::terminate();
        }
    }

    [[nodiscard]] std::shared_ptr<void> try_acquire(const std::shared_ptr<event_loop_state>& owner) {
        {
            const std::lock_guard lock(mutex_);
            if (finalized_ || root_admission_closed_ || sealed_) {
                return {};
            }
            ++pending_;
        }
        try {
            auto token = std::make_shared<lease_token_type>(owner);
            return std::shared_ptr<void>(std::move(token), this);
        } catch (...) {
            finish();
            throw;
        }
    }

    void finish() noexcept {
        bool claimed = false;
        {
            const std::lock_guard lock(mutex_);
            if (pending_ == 0) {
                std::terminate();
            }
            --pending_;
            claimed = claim_finalize_locked();
        }
        if (claimed) {
            run_claimed_finalize();
        }
    }

    [[nodiscard]] std::size_t pending() const noexcept {
        const std::lock_guard lock(mutex_);
        return pending_;
    }

    void close_admission() noexcept {
        bool abandoned = false;
        {
            const std::lock_guard lock(mutex_);
            root_admission_closed_ = true;
            sealed_ = true;
            // An installing begin_stop() observes finalized_ and drops its own
            // callable; a claimed one already belongs to its claimer.
            abandoned = !finalized_ && finalize_slot_ == finalize_slot_type::installed;
            finalized_ = true;
        }
        if (abandoned) {
            finalize_ = nullptr;
        }
    }

    void seal() noexcept {
        bool claimed = false;
        {
            const std::lock_guard lock(mutex_);
            sealed_ = true;
            claimed = claim_finalize_locked();
        }
        if (claimed) {
            run_claimed_finalize();
        }
    }

private:
    enum class finalize_slot_type : unsigned char {
        empty,
        // Reserved by begin_stop(), which moves the callable in unlocked.
        installing,
        installed,
    };

    // Drain, seal, and a publishing begin_stop() claim finalization under the
    // gate mutex; a slot still being installed is claimed by its installer.
    [[nodiscard]] bool claim_finalize_locked() noexcept {
        if (!sealed_ || pending_ != 0 || finalized_ || finalize_slot_ == finalize_slot_type::installing) {
            return false;
        }
        finalized_ = true;
        return true;
    }

    // The claim (or the abandonment) makes the caller the only thread that
    // touches finalize_, so it is moved and invoked without the gate mutex.
    void run_claimed_finalize() noexcept {
        auto finalize = std::move(finalize_);
        if (finalize) {
            finalize();
        }
    }

    struct lease_token_type final {
        explicit lease_token_type(std::shared_ptr<event_loop_state> owner_value) noexcept :
            owner_(std::move(owner_value)) {}
        ~lease_token_type();
        std::shared_ptr<event_loop_state> owner_;
    };

    mutable std::mutex mutex_;
    std::size_t pending_{0};
    bool root_admission_closed_{false};
    bool sealed_{false};
    bool finalized_{false};
    finalize_slot_type finalize_slot_{finalize_slot_type::empty};
    move_only_function<void()> finalize_;
};

// The single Ruvia attachment claim on a caller-owned io_context.
class external_context_claim final {
public:
    explicit external_context_claim(asio::io_context& io_context);
    ~external_context_claim();

    external_context_claim(const external_context_claim&) = delete;
    external_context_claim& operator=(const external_context_claim&) = delete;

    // Hands the state to the context service, which retires it at the latest
    // when the context shuts down.
    void retain(std::shared_ptr<event_loop_state> state_value) noexcept;
    // Releases the retained state and the claim once the state has retired.
    void release(event_loop_state* state_value) noexcept;

private:
    external_context_attachment_service* service_;
    bool retained_{false};
};

// A caller-owned io_context bound to one loop: its claim, the worker endpoint,
// and the same worker stop chain that a pool loop's worker_runtime owns.
struct attached_context final {
    attached_context(asio::io_context& io_context, std::size_t queue_capacity, blocking_pool* pool, move_only_function<void(std::exception_ptr)> failure);

    external_context_claim claim_;
    worker_runtime_context runtime_;
    worker_stop_chain stop_;
    move_only_function<void(std::exception_ptr)> failure_;
};

// A standalone worker_runtime that exposes its loop through
// worker_runtime::loop(); the runtime outlives every handle to the loop.
struct borrowed_runtime final {
    worker_runtime* runtime_;
};

// One loop's runtime and retirement. A pool loop owns a worker_runtime; an
// attached loop owns the same worker stop chain over a caller-owned context;
// a borrowed loop gates the stop admission of a standalone worker_runtime.
// Both stop through stop(): it seals the retirement gate on the stopping
// thread and requests the worker stop chain, whose owner-thread stop control
// re-enters stop(); the drained gate finalizes the chain on the owner.
struct event_loop_state final : std::enable_shared_from_this<event_loop_state> {
    using context_ownership_type = std::variant<worker_runtime, attached_context, borrowed_runtime>;

    event_loop_state(std::size_t queue_capacity, blocking_pool* pool);
    explicit event_loop_state(worker_runtime& runtime);
    event_loop_state(asio::io_context& external_context, std::size_t queue_capacity, blocking_pool* pool, move_only_function<void(std::exception_ptr)> failure);

    [[nodiscard]] worker_runtime& owned_runtime() noexcept {
        return *std::get_if<worker_runtime>(&context_ownership_);
    }
    // Pool loops only: the runtime's first failure, without observing it.
    [[nodiscard]] std::exception_ptr recorded_failure() noexcept {
        return owned_runtime().recorded_failure();
    }

    // Attached loops only: hooks the stop chain and hands the state to the
    // context service, which retires it at the latest when the context shuts
    // down.
    void attach(const std::shared_ptr<event_loop_state>& self);
    void install_shutdown_registration();

    void worker_stopping_complete() noexcept;

    // Delivers to the bound failure sink; only a state whose sink is not yet
    // bound (pool construction rollback) reports under `unbound_context`.
    void route_failure(std::exception_ptr failure, const char* unbound_context) noexcept;
    void report_attached_failure(std::exception_ptr failure) noexcept;
    [[noreturn]] void fail_teardown_start(std::exception_ptr failure) noexcept;

    void stop() noexcept;
    void complete_stop_admission() noexcept;
    void finalize_stop() noexcept;
    // Asio service shutdown of an attached context: retires the worker before
    // the context destroys its scheduler.
    void shutdown_external_context() noexcept;

    // First member: unlinks from the worker registry after every other member is gone.
    worker_shutdown_registration shutdown_registration_;
    context_ownership_type context_ownership_;
    asio::io_context* const io_context_;
    asio::io_context::executor_type executor_;
    worker_runtime_context& runtime_;
    std::atomic_bool stopping_{false};
    // Bound once before publication to the stable pool owner or weak attachment
    // owner. Root tasks and stop callbacks borrow it through the loop state
    // instead of copying it.
    move_only_function<void(std::exception_ptr)> failure_sink_;
    event_loop_retirement retirement_;

private:
    void schedule_finalize_when_idle() noexcept;
    void defer_finalize_stop(std::shared_ptr<event_loop_state> self) noexcept;
};

inline event_loop_retirement::lease_token_type::~lease_token_type() {
    owner_->retirement_.finish();
}

}  // namespace ruvia::detail
