#pragma once

#include <atomic>
#include <cstddef>
#include <exception>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <optional>
#include <variant>

#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>

#include "ruvia/core/detail/worker/worker_dispatcher.h"
#include "ruvia/core/memory/process_resource.h"
#include "ruvia/core/move_only_function.h"
#include "ruvia/core/worker_runtime.h"
#include "ruvia/core/worker_runtime_context.h"

namespace ruvia::detail {

class external_context_attachment_service;
struct event_loop_state;

// Retirement gate of one loop: root tasks, stop callbacks, and cleanup each
// hold a pending count; finalize runs once the gate is sealed and drained.
class event_loop_retirement final {
public:
    event_loop_retirement() = default;

    void set_finalize(move_only_function<void()> finalize) noexcept {
        const std::lock_guard lock(mutex_);
        finalize_ = std::move(finalize);
    }

    [[nodiscard]] bool try_begin() noexcept {
        const std::lock_guard lock(mutex_);
        if (sealed_ || finalized_) {
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

    [[nodiscard]] std::shared_ptr<void> try_acquire(bool root_admission = true) {
        {
            const std::lock_guard lock(mutex_);
            if (finalized_ || (root_admission && (root_admission_closed_ || sealed_))) {
                return {};
            }
            ++pending_;
        }
        try {
            auto token = std::allocate_shared<lease_token_type>(
                std::pmr::polymorphic_allocator<lease_token_type>(process_resource()), this);
            return std::shared_ptr<void>(std::move(token), this);
        } catch (...) {
            finish();
            throw;
        }
    }

    void finish() noexcept {
        move_only_function<void()> finalize;
        {
            const std::lock_guard lock(mutex_);
            if (pending_ == 0) {
                std::terminate();
            }
            --pending_;
            if (sealed_ && pending_ == 0 && !finalized_) {
                finalized_ = true;
                finalize = std::move(finalize_);
            }
        }
        if (finalize) {
            finalize();
        }
    }

    [[nodiscard]] std::size_t pending() const noexcept {
        const std::lock_guard lock(mutex_);
        return pending_;
    }

    void close_root_admission() noexcept {
        const std::lock_guard lock(mutex_);
        root_admission_closed_ = true;
    }

    void close_admission() noexcept {
        const std::lock_guard lock(mutex_);
        root_admission_closed_ = true;
        sealed_ = true;
    }

    void seal() noexcept {
        move_only_function<void()> finalize;
        {
            const std::lock_guard lock(mutex_);
            sealed_ = true;
            if (pending_ == 0 && !finalized_) {
                finalized_ = true;
                finalize = std::move(finalize_);
            }
        }
        if (finalize) {
            finalize();
        }
    }

private:
    struct lease_token_type final {
        explicit lease_token_type(event_loop_retirement* owner_value) noexcept
            : owner_(owner_value) {}
        ~lease_token_type() {
            owner_->finish();
        }
        event_loop_retirement* owner_;
    };

    mutable std::mutex mutex_;
    std::size_t pending_{0};
    bool root_admission_closed_{false};
    bool sealed_{false};
    bool finalized_{false};
    move_only_function<void()> finalize_;
};

// The single Ruvia attachment claim on a caller-owned io_context
// (event_loop_attachment.cpp).
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

// One loop's runtime and retirement. A pool loop owns its worker_runtime; an
// attached loop claims a caller-owned io_context and drives its own retirement
// on the stopping thread, because that context may never run again.
struct event_loop_state final : worker_shutdown_listener,
                                std::enable_shared_from_this<event_loop_state> {
    using context_ownership_type = std::variant<worker_runtime, external_context_claim>;

    explicit event_loop_state(std::size_t queue_capacity);
    event_loop_state(asio::io_context& external_context, std::size_t queue_capacity);

    [[nodiscard]] worker_runtime& owned_runtime() noexcept {
        return *std::get_if<worker_runtime>(&context_ownership_);
    }

    void retain_external_context(const std::shared_ptr<event_loop_state>& self) noexcept;
    void install_lifecycle_listener();

    void worker_stopping() noexcept override {}
    void worker_stopping_complete() noexcept override;

    void report_failure(std::exception_ptr failure) noexcept;
    void report_attached_failure(std::exception_ptr failure) noexcept;
    [[noreturn]] void fail_teardown_start(std::exception_ptr failure) noexcept;

    void stop(bool runtime_started, std::shared_ptr<event_loop_state> keep_alive) noexcept;
    void finalize_stop() noexcept;
    // Asio service shutdown of an attached context: retires the worker before
    // the context destroys its scheduler (event_loop_attachment.cpp).
    void shutdown_external_context() noexcept;

    context_ownership_type context_ownership_;
    asio::io_context* const io_context_;
    asio::io_context::executor_type executor_;
    std::optional<asio::executor_work_guard<asio::io_context::executor_type>> work_;
    std::optional<worker_runtime_context> attached_runtime_;
    worker_runtime_context& runtime_;
    std::atomic_bool stopping_{false};
    // Bound once before publication to the stable pool owner or weak attachment
    // owner. Root tasks and stop callbacks borrow it through the loop state
    // instead of copying it.
    move_only_function<void(std::exception_ptr)> failure_sink_;
    std::shared_ptr<event_loop_retirement> retirement_{std::make_shared<event_loop_retirement>()};
};

}  // namespace ruvia::detail
