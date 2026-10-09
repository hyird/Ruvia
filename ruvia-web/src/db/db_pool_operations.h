#pragma once

#include <array>
#include <charconv>
#include <chrono>
#include <exception>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <asio/ip/tcp.hpp>

#include "ruvia/core/async.h"
#include "ruvia/core/operation_timeout.h"
#include "ruvia/core/pool_lease_scheduler.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/task.h"
#include "ruvia/core/worker_cancellation.h"
#include "ruvia/core/worker_handle.h"
#include "ruvia/core/worker_timer.h"
#include "ruvia/web/db/db_rows.h"
#include "ruvia/web/db/db_transaction.h"
#include "ruvia/web/db/db_types.h"

#include "db/db_host_resolution.h"
#include "db/db_transaction_start.h"

// The parts of a pooled database operation that do not depend on the driver:
// which slot it runs on, and what happens to that slot when it fails.

namespace ruvia::detail {

enum class db_slot_abort_reason : std::uint8_t {
    none,
    cancelled,
};

template <typename pool_type>
using db_cancellation_target = worker_cancellation_target<pool_type>;

template <typename pool_type>
class db_slot_cancellation_guard final {
public:
    db_slot_cancellation_guard(pool_type& pool, std::size_t slot, const stop_token& stop_token_value)
        : cancellation_(pool.cancellation_target_, pool.slots_[slot].cancellation_id_) {
        auto& connection = pool.slots_[slot];
        connection.abort_reason_ = db_slot_abort_reason::none;
        cancellation_.arm(stop_token_value);
    }

    db_slot_cancellation_guard(const db_slot_cancellation_guard&) = delete;
    db_slot_cancellation_guard& operator=(const db_slot_cancellation_guard&) = delete;

    ~db_slot_cancellation_guard() {
        finish();
    }

    void finish() noexcept {
        cancellation_.reset();
    }

private:
    worker_cancellation_registration<db_cancellation_target<pool_type>> cancellation_;
};

// A pool slot held for the duration of one operation. Releasing it is the
// caller's obligation however the operation ends, so the guard owns that: the
// driver code between acquire and release can throw freely.
template <typename pool_type>
class db_slot_guard final {
public:
    db_slot_guard(pool_type& pool, std::size_t slot) noexcept
        : pool_(&pool),
          slot_(slot) {}
    db_slot_guard(const db_slot_guard&) = delete;
    db_slot_guard& operator=(const db_slot_guard&) = delete;
    ~db_slot_guard() {
        if (pool_ != nullptr) {
            pool_->release_slot(slot_);
        }
    }

private:
    pool_type* pool_;
    std::size_t slot_;
};

template <typename slot_type>
class db_slot_active_wait_guard final {
public:
    explicit db_slot_active_wait_guard(slot_type& slot) noexcept
        : slot_(slot) {
        if (slot_.wait_active_) {
            std::terminate();
        }
        slot_.wait_active_ = true;
    }

    db_slot_active_wait_guard(const db_slot_active_wait_guard&) = delete;
    db_slot_active_wait_guard& operator=(const db_slot_active_wait_guard&) = delete;

    ~db_slot_active_wait_guard() {
        slot_.wait_active_ = false;
    }

private:
    slot_type& slot_;
};

// Slots keep their existing address-stable timer registration. Only the expiry
// action is driver-specific; installation, rollback and retirement are shared.
template <typename slot_type>
void clear_db_slot_deadline(slot_type& slot) noexcept {
    slot.deadline_timer_->cancel();
    (void)slot.deadline_.clear();
    if constexpr (requires { slot.deadline_continuation_; }) {
        slot.deadline_continuation_ = {};
    }
}

template <typename slot_type, typename deadline_kind, auto expiry_action = &slot_type::expire_deadline>
void arm_db_slot_deadline(const worker_handle& worker_value, slot_type& slot,
    std::chrono::milliseconds timeout, deadline_kind kind) {
    static_assert(noexcept(expiry_action(slot, kind)));
    clear_db_slot_deadline(slot);
    if (timeout.count() <= 0) {
        return;
    }
    const auto deadline_value = worker_timer_deadline_after(timeout);
    slot.deadline_.arm(deadline_value, kind);
    try {
        worker_value.schedule_timer(*slot.deadline_timer_, deadline_value,
            [&slot](worker_timer_outcome outcome) noexcept {
                if (outcome != worker_timer_outcome::expired) {
                    return;
                }
                const auto expired = slot.deadline_.expire(std::chrono::steady_clock::now());
                if (expired.has_value()) {
                    expiry_action(slot, *expired);
                }
            });
    } catch (...) {
        slot.deadline_timer_->cancel_quietly();
        slot.deadline_.reset();
        throw;
    }
}

// Taking and giving back a slot is pure lease bookkeeping: no driver is
// involved, so both pools share these. A release that names no live lease is a
// bug in the caller, not a runtime condition, and cannot be reported through a
// noexcept path.
template <typename pool_type>
task<std::size_t> acquire_db_slot(pool_type& pool, ruvia::operation_timeout timeout, stop_token stop_token_value) {
    const auto acquire_timeout = timeout.constrained_by(pool.config_.acquire_timeout_).remaining();
    const auto result_value =
        co_await pool.scheduler_.acquire(acquire_timeout, std::move(stop_token_value));
    switch (result_value.status()) {
        case pool_waiter_result::status_type::acquired:
            co_return result_value.index();
        case pool_waiter_result::status_type::timed_out:
            throw db_error(db_error::code_type::timeout, pool.config_.driver_,
                "database connection pool acquire timed out");
        case pool_waiter_result::status_type::cancelled:
            throw db_error(
                db_error::code_type::cancelled, pool.config_.driver_, "database operation cancelled");
        case pool_waiter_result::status_type::closed:
            throw db_error(
                db_error::code_type::closing, pool.config_.driver_, "database client is closing");
    }
    std::terminate();
}

template <typename pool_type>
void release_db_slot(pool_type& pool, std::size_t slot) noexcept {
    const auto status = pool.scheduler_.release(slot);
    if (status == pool_lease_release_status::invalid_slot ||
        status == pool_lease_release_status::already_released) {
        std::terminate();
    }
}

// Driver pools compose this worker-affine lifecycle policy. It borrows an
// address-stable backend; slot connect/close stay driver-specific, while lease
// scheduling and cancellation have one implementation.
template <typename pool_type>
class db_pool_lifecycle final {
public:
    explicit db_pool_lifecycle(pool_type& owner_value) noexcept
        : owner_(owner_value) {}

    db_pool_lifecycle(const db_pool_lifecycle&) = delete;
    db_pool_lifecycle& operator=(const db_pool_lifecycle&) = delete;
    db_pool_lifecycle(db_pool_lifecycle&&) = delete;
    db_pool_lifecycle& operator=(db_pool_lifecycle&&) = delete;

    [[nodiscard]] task<void> connect() {
        auto& pool = owner_;
        const ruvia::operation_timeout operation_timeout(std::nullopt);
        for (auto& slot : pool.slots_) {
            co_await pool.connect_unlocked(slot, operation_timeout);
        }
    }

    void close_now() noexcept {
        auto& pool = owner_;
        (void)pool.scheduler_.close();
        for (auto& slot : pool.slots_) {
            pool.close_slot(slot);
        }
    }

    [[nodiscard]] task<std::size_t> acquire_slot(ruvia::operation_timeout timeout, stop_token stop_token_value) {
        return acquire_db_slot(owner_, timeout, std::move(stop_token_value));
    }

    void release_slot(std::size_t slot) noexcept {
        release_db_slot(owner_, slot);
    }

    void cancel_operation_by_id(std::uint64_t cancellation_id) noexcept {
        auto& pool = owner_;
        for (auto& slot : pool.slots_) {
            if (slot.cancellation_id_ != cancellation_id) {
                continue;
            }
            slot.abort_reason_ = db_slot_abort_reason::cancelled;
            pool.close_slot(slot);
            return;
        }
    }

    template <typename slot_type>
    void throw_if_cancelled(const slot_type& slot) const {
        if (slot.abort_reason_ == db_slot_abort_reason::cancelled) {
            throw db_error(
                db_error::code_type::cancelled, owner_.config_.driver_, "database operation cancelled");
        }
    }

private:
    pool_type& owner_;
};

// Ending a transaction is the same for every driver: run the control statement
// on the slot the transaction holds, and release that slot exactly once. A
// failure closes the connection before releasing, because a slot whose COMMIT
// or ROLLBACK did not complete cannot be handed to the next caller.
//
// `Pool` supplies slots_, execute_control(), close_slot() and release_slot(); it
// declares this a friend so the shared rule stays out of the drivers.
template <typename pool_type>
task<void> finish_db_transaction(pool_type& pool, std::size_t slot, std::string_view command,
    std::pmr::memory_resource* resource, const operation_options& options) {
    if (slot >= pool.slots_.size()) {
        throw std::logic_error("database transaction slot is invalid");
    }
    const ruvia::operation_timeout operation_timeout_value(options.timeout_);
    db_slot_cancellation_guard cancellation(pool, slot, options.stop_token_);
    try {
        co_await pool.execute_control(pool.slots_[slot], command, resource, operation_timeout_value);
    } catch (...) {
        pool.close_slot(pool.slots_[slot]);
        cancellation.finish();
        pool.release_slot(slot);
        throw;
    }
    cancellation.finish();
    pool.release_slot(slot);
}

// Starting a transaction has the same lease transfer for every driver. The
// backend control statement owns cancellation checks and connect-on-demand;
// duplicating those here would run the same policy twice on the call chain.
template <typename pool_type>
task<db_transaction> begin_db_transaction(pool_type& pool, std::pmr::memory_resource* resource,
    operation_options operation_options_value,
    db_transaction_start_plan plan) {
    const ruvia::operation_timeout operation_timeout_value(operation_options_value.timeout_);
    const auto slot_index = co_await pool.acquire_slot(operation_timeout_value, operation_options_value.stop_token_);
    db_slot_cancellation_guard cancellation(pool, slot_index, operation_options_value.stop_token_);
    try {
        if (!plan.configure_.empty()) {
            co_await pool.execute_control(pool.slots_[slot_index], plan.configure_, resource, operation_timeout_value);
        }
        co_await pool.execute_control(pool.slots_[slot_index], plan.begin_, resource, operation_timeout_value);
        co_return db_transaction(db_pool_ref_type{&pool}, slot_index, resource, std::move(operation_options_value));
    } catch (...) {
        pool.close_slot(pool.slots_[slot_index]);
        cancellation.finish();
        pool.release_slot(slot_index);
        throw;
    }
}

// A statement inside an open transaction: it runs on the slot the transaction
// already holds, so it neither acquires nor releases one. A failure closes the
// connection and gives the slot back, because a transaction whose statement
// failed mid-protocol cannot continue on it.
template <typename pool_type>
task<db_rows> query_on_db_transaction_slot(pool_type& pool, std::size_t slot, std::pmr::string sql,
    std::pmr::vector<db_value> params, std::pmr::memory_resource* resource,
    const operation_options& options) {
    if (slot >= pool.slots_.size()) {
        throw std::logic_error("database transaction slot is invalid");
    }
    const ruvia::operation_timeout operation_timeout_value(options.timeout_);
    db_slot_cancellation_guard cancellation(pool, slot, options.stop_token_);
    try {
        co_return co_await pool.query_on_slot(
            pool.slots_[slot], sql, std::span<const db_value>(params), resource, operation_timeout_value);
    } catch (...) {
        pool.close_slot(pool.slots_[slot]);
        cancellation.finish();
        pool.release_slot(slot);
        throw;
    }
}

template <typename pool_type>
task<db_exec_result> execute_on_db_transaction_slot(pool_type& pool, std::size_t slot, std::pmr::string sql,
    std::pmr::vector<db_value> params, std::pmr::memory_resource* resource,
    const operation_options& options) {
    if (slot >= pool.slots_.size()) {
        throw std::logic_error("database transaction slot is invalid");
    }
    const ruvia::operation_timeout operation_timeout_value(options.timeout_);
    db_slot_cancellation_guard cancellation(pool, slot, options.stop_token_);
    try {
        co_return co_await pool.execute_on_slot(
            pool.slots_[slot], sql, std::span<const db_value>(params), resource, operation_timeout_value);
    } catch (...) {
        pool.close_slot(pool.slots_[slot]);
        cancellation.finish();
        pool.release_slot(slot);
        throw;
    }
}

// One buffered statement on a pooled connection. The shape is the same for every
// driver: refuse empty SQL before taking a slot, hold the slot for the duration,
// and close the connection if the statement throws -- a slot whose statement
// failed mid-protocol cannot be reused. The guard releases the slot either way.
template <typename pool_type>
task<db_rows> execute_db_query(pool_type& pool, std::pmr::string sql, std::pmr::vector<db_value> params,
    std::pmr::memory_resource* resource, operation_options options) {
    if (sql.empty()) {
        throw std::invalid_argument("SQL must not be empty");
    }

    const ruvia::operation_timeout operation_timeout_value(options.timeout_);
    const auto slot_index = co_await pool.acquire_slot(operation_timeout_value, options.stop_token_);
    typename pool_type::slot_guard_type guard(pool, slot_index);
    db_slot_cancellation_guard cancellation(pool, slot_index, options.stop_token_);
    try {
        co_return co_await pool.query_on_slot(pool.slots_[slot_index], sql,
            std::span<const db_value>(params), resource, operation_timeout_value);
    } catch (...) {
        pool.close_slot(pool.slots_[slot_index]);
        throw;
    }
}

template <typename pool_type>
task<db_exec_result> execute_db_command(pool_type& pool, std::pmr::string sql,
    std::pmr::vector<db_value> params, std::pmr::memory_resource* resource,
    operation_options options) {
    if (sql.empty()) {
        throw std::invalid_argument("SQL must not be empty");
    }

    const ruvia::operation_timeout operation_timeout_value(options.timeout_);
    const auto slot_index = co_await pool.acquire_slot(operation_timeout_value, options.stop_token_);
    typename pool_type::slot_guard_type guard(pool, slot_index);
    db_slot_cancellation_guard cancellation(pool, slot_index, options.stop_token_);
    try {
        co_return co_await pool.execute_on_slot(pool.slots_[slot_index], sql,
            std::span<const db_value>(params), resource, operation_timeout_value);
    } catch (...) {
        pool.close_slot(pool.slots_[slot_index]);
        throw;
    }
}

// The pool's configured port as a NUL-terminated buffer, the form asio's
// resolver takes it in.
[[nodiscard]] inline std::array<char, 6> format_db_port(
    std::uint16_t port, std::string_view backend) {
    std::array<char, 6> output{};
    const auto parsed_value = std::to_chars(output.data(), output.data() + output.size() - 1, port);
    if (parsed_value.ec != std::errc{}) {
        throw std::logic_error(std::string("failed to format ").append(backend).append(" port"));
    }
    *parsed_value.ptr = '\0';
    return output;
}

// Resolving the configured host is the same for every driver: asio's resolver
// under the operation deadline, with the slot's resolve deadline armed around
// it so a stalled resolve is torn down with everything else on that slot. The
// deadline is checked once before arming and once after resuming, because the
// timer may have fired while this coroutine was suspended.
//
// Only the backend's name in the diagnostics differs, so it is the argument.
// `Pool` supplies config_, resource_ and worker_;
// it declares this a friend so the shared rule stays out of the drivers.
template <typename pool_type, typename slot_type>
task<db_resolved_addresses_type> resolve_db_host(
    pool_type& pool, slot_type& slot, ruvia::operation_timeout deadline_value, std::string_view backend) {
    const auto timed_out = [&pool, backend] {
        return db_error(db_error::code_type::timeout, pool.config_.driver_,
            std::string(backend).append(" host resolve timed out"));
    };

    pool.throw_if_cancelled(slot);
    const auto remaining = deadline_value.remaining();
    if (remaining.has_value() && remaining->count() <= 0) {
        throw timed_out();
    }
    if (remaining.has_value()) {
        arm_db_slot_deadline(pool.worker_, slot, *remaining, slot_type::deadline_kind_type::resolve);
    } else {
        clear_db_slot_deadline(slot);
    }

    db_slot_active_wait_guard active_resolve(slot);

    const auto port = format_db_port(pool.config_.port_, backend);
    try {
        auto completion = co_await ruvia::async_asio<asio::ip::tcp::resolver::results_type>(
            [&pool, &slot, &port](auto handler) mutable {
                slot.resolver_.async_resolve(
                    pool.config_.host_, std::string_view(port.data()), std::move(handler));
            });
        const auto resolve_error = completion.error_code();
        auto results = std::move(completion).take_result();
        const auto after_resolve = deadline_value.remaining();
        const bool slot_deadline_expired = slot.deadline_.expired();
        clear_db_slot_deadline(slot);
        const bool deadline_expired =
            slot_deadline_expired || (after_resolve.has_value() && after_resolve->count() <= 0);
        pool.throw_if_cancelled(slot);
        if (slot.close_requested_) {
            throw db_error(
                db_error::code_type::closing, pool.config_.driver_, "database client is closing");
        }
        if (deadline_expired) {
            throw timed_out();
        }
        if (resolve_error) {
            throw db_error(db_error::code_type::resolve_failed, pool.config_.driver_,
                std::system_error(
                    resolve_error, std::string("resolving ").append(backend).append(" host failed"))
                    .what(),
                resolve_error.value());
        }
        co_return collect_db_resolved_addresses(results, pool.config_.driver_, pool.resource_);
    } catch (...) {
        clear_db_slot_deadline(slot);
        throw;
    }
}

}  // namespace ruvia::detail
