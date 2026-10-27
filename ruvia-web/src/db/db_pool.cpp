#include "db/db_pool.h"

#include <array>
#include <exception>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <variant>

#include "ruvia/core/async.h"
#include "ruvia/core/failure_report.h"
#include "ruvia/http/http_number_format.h"

#include "db/db_transaction_start.h"

namespace ruvia::detail {
namespace {

[[nodiscard]] const worker_handle& require_db_worker(const worker_handle& worker_value) {
    if (!worker_value.valid()) {
        throw std::invalid_argument("database pool requires a valid worker");
    }
    return worker_value;
}

[[nodiscard]] db_error db_wait_failure(db_driver driver, std::exception_ptr failure) {
    try {
        std::rethrow_exception(std::move(failure));
    } catch (const db_error& error) {
        return error;
    } catch (const std::system_error& error) {
        return db_error(db_error::code_type::io_error, driver, system_error_text(error), error.code().value());
    } catch (const std::exception& error) {
        return db_error(db_error::code_type::io_error, driver, error.what());
    }
}

// Whether a slot can serve the next statement after this failure. A driver
// reports a statement the server rejected, or a result shape the caller got
// wrong, only once the command has been drained and the connection is idle
// again -- closing the slot itself when it cannot get there -- so those leave
// the connection reusable. Every other failure may have interrupted an
// exchange, and the slot is closed.
[[nodiscard]] bool db_slot_reusable_after(const std::exception_ptr& failure) noexcept {
    try {
        std::rethrow_exception(failure);
    } catch (const db_error& error) {
        return error.code() == db_error::code_type::statement_failed;
    } catch (const std::invalid_argument&) {
        return true;
    } catch (...) {
        return false;
    }
}

}  // namespace

std::array<char, 6> format_db_port(std::uint16_t port) {
    std::array<char, http_max_unsigned_decimal_size> digits;
    const auto text = http_format_unsigned(digits, port);
    // Zero initialization NUL-terminates the at most five port digits.
    std::array<char, 6> output{};
    text.copy(output.data(), text.size());
    return output;
}

template <typename driver_type>
void db_pool<driver_type>::slot_policy::throw_acquire_failure(const db_pool& pool, worker_wait_status status) {
    switch (status) {
        case worker_wait_status::timed_out:
            throw db_error(db_error::code_type::timeout, pool.config_.driver_,
                "database connection pool acquire timed out");
        case worker_wait_status::cancelled:
            throw db_error(db_error::code_type::cancelled, pool.config_.driver_, "database operation cancelled");
        case worker_wait_status::closed:
            throw pool.closing_error();
        case worker_wait_status::value:
        case worker_wait_status::worker_stopping:
            break;
    }
    std::terminate();
}

template <typename driver_type>
void db_pool<driver_type>::slot_policy::throw_aborted(const db_pool& pool, pool_slot_abort_reason reason) {
    if (reason == pool_slot_abort_reason::cancelled) {
        throw db_error(db_error::code_type::cancelled, pool.config_.driver_, "database operation cancelled");
    }
    throw pool.closing_error();
}

template <typename driver_type>
void db_pool<driver_type>::slot_policy::expire_slot(db_pool& pool, std::size_t index, db_deadline_kind kind) noexcept {
    auto& slot = pool.slots_[index];
    switch (kind) {
        case db_deadline_kind::resolve:
            slot.resolver_.cancel();
            return;
        case db_deadline_kind::socket:
            slot.wait_socket_.cancel();
            return;
        case db_deadline_kind::sleep:
            if (auto handle = std::exchange(slot.deadline_continuation_, {})) {
                handle.resume();
            }
            return;
    }
}

template <typename driver_type>
void db_pool<driver_type>::slot_policy::cancel_slot(db_pool& pool, std::size_t index) noexcept {
    pool.close_slot(index);
}

template <typename driver_type>
db_pool<driver_type>::db_pool(asio::io_context& io_context, const worker_handle& worker_value,
    db_config_storage config, std::pmr::memory_resource* resource)
    : io_context_(io_context),
      config_(std::move(config)),
      resource_(pmr_resource_or_default(resource)),
      worker_(require_db_worker(worker_value)),
      slots_(resource_),
      leases_(*this, config_.pool_size_per_worker_, worker_, resource_),
      state_(*this, worker_) {
    if (config_.driver_ != driver_type::driver) {
        throw std::invalid_argument(std::string(driver_type::name).append(" pool requires its own driver"));
    }
    slots_.reserve(config_.pool_size_per_worker_);
    for (std::size_t index = 0; index < config_.pool_size_per_worker_; ++index) {
        slots_.emplace_back(io_context_, worker_, resource_);
    }
}

template <typename driver_type>
db_pool<driver_type>::~db_pool() {
    close_now();
}

template <typename driver_type>
db_error db_pool<driver_type>::timed_out() const {
    return db_error(db_error::code_type::timeout, driver_type::driver,
        std::string(driver_type::name).append(" operation timed out"));
}

template <typename driver_type>
db_error db_pool<driver_type>::closing_error() const {
    return db_error(db_error::code_type::closing, driver_type::driver, "database client is closing");
}

template <typename driver_type>
task<void> db_pool<driver_type>::connect() {
    const operation_timeout timeout(std::nullopt);
    for (std::size_t index = 0; index < slots_.size(); ++index) {
        co_await connect_slot(index, timeout);
    }
}

template <typename driver_type>
void db_pool<driver_type>::close_now() noexcept {
    (void)leases_.close();
    state_.close();
    for (std::size_t index = 0; index < slots_.size(); ++index) {
        close_slot(index);
    }
}

template <typename driver_type>
void db_pool<driver_type>::release_slot(std::size_t index) noexcept {
    if (!state_.take_released_slot(index)) {
        leases_.release(index);
    }
}

template <typename driver_type>
std::optional<std::uint64_t> db_pool<driver_type>::oldest_exclusive_arrival() const noexcept {
    for (const auto* waiting = oldest_exclusive_; waiting != nullptr; waiting = waiting->next_) {
        if (!waiting->stop_.stop_requested()) {
            return waiting->arrival_;
        }
    }
    return std::nullopt;
}

// Stamped and listed before its acquire is queued: the lease scheduler serves
// its waiters in the same order.
template <typename driver_type>
db_pool<driver_type>::exclusive_wait::exclusive_wait(db_pool& pool, const stop_token& stop) noexcept
    : pool_(pool),
      stop_(stop),
      arrival_(pool.next_arrival()),
      previous_(pool.newest_exclusive_) {
    if (previous_ != nullptr) {
        previous_->next_ = this;
    } else {
        pool_.oldest_exclusive_ = this;
    }
    pool_.newest_exclusive_ = this;
}

template <typename driver_type>
db_pool<driver_type>::exclusive_wait::~exclusive_wait() {
    if (previous_ != nullptr) {
        previous_->next_ = next_;
    } else {
        pool_.oldest_exclusive_ = next_;
    }
    if (next_ != nullptr) {
        next_->previous_ = previous_;
    } else {
        pool_.newest_exclusive_ = previous_;
    }
    pool_.state_.exclusive_waiter_left();
}

template <typename driver_type>
void db_pool<driver_type>::close_slot(std::size_t index) noexcept {
    auto& slot = slots_[index];
    slot.close_requested_ = true;
    slot.resolver_.cancel();
    if (slot.wait_active_) {
        const auto* kind = leases_.deadline_kind(index);
        if (kind != nullptr && *kind == db_deadline_kind::sleep) {
            if (auto handle = std::exchange(slot.deadline_continuation_, {})) {
                handle.resume();
            }
        } else {
            // The wait handlers borrow the wrapper and the driver connection.
            // Cancellation wakes the waiting coroutine, which completes the
            // close once its operation unwinds.
            slot.wait_socket_.cancel();
        }
        return;
    }

    (void)leases_.clear_deadline(index);
    slot.deadline_continuation_ = {};
    // Closing the duplicate first leaves the driver's handle as the last
    // reference, so the driver's close still ends the connection.
    slot.wait_socket_.close();
    if (slot.connection_ != nullptr) {
        driver_type::close_connection(*slot.connection_);
        slot.connection_ = nullptr;
    }
    slot.connected_ = false;
    slot.close_requested_ = false;
}

template <typename driver_type>
task<void> db_pool<driver_type>::connect_slot(std::size_t index, const operation_timeout& timeout) {
    if (leases_.closing()) {
        throw closing_error();
    }
    auto& slot = slots_[index];
    if (slot.connected_) {
        co_return;
    }
    leases_.throw_if_aborted(index);
    const operation_timeout deadline_value = timeout.constrained_by(config_.connect_timeout_);
    try {
        co_await driver_type::connect(*this, index, deadline_value);
        slot.connected_ = true;
    } catch (...) {
        close_slot(index);
        throw;
    }
}

// Resolving runs under the operation deadline with the slot's resolve deadline
// armed around it. A system query already running cannot be interrupted, but
// cancellation retires this slot's waiter immediately and discards its late
// result. The deadline is checked once before arming and once after resuming,
// because the timer may have fired while this coroutine was suspended.
template <typename driver_type>
task<db_resolved_addresses_type> db_pool<driver_type>::resolve_host(
    std::size_t index, const operation_timeout& deadline_value) {
    const auto resolve_timed_out = [] {
        return db_error(db_error::code_type::timeout, driver_type::driver,
            std::string(driver_type::name).append(" host resolve timed out"));
    };

    leases_.throw_if_aborted(index);
    if (!leases_.arm_deadline(index, deadline_value.deadline(), db_deadline_kind::resolve)) {
        throw resolve_timed_out();
    }
    auto& slot = slots_[index];
    try {
        db_slot_active_wait_guard active_resolve(slot);
        client_resolve_result completion;
        {
            auto pending = slot.resolver_.resolve(config_.host_, config_.port_, deadline_value);
            if (auto* immediate = std::get_if<client_resolve_result>(&pending)) {
                completion = std::move(*immediate);
            } else {
                completion = co_await std::move(std::get<task<client_resolve_result>>(pending));
            }
        }
        const auto resolve_error = completion.error_;
        const bool deadline_expired = leases_.clear_deadline(index) || deadline_value.expired();
        leases_.throw_if_aborted(index);
        if (slot.close_requested_) {
            throw closing_error();
        }
        if (deadline_expired) {
            throw resolve_timed_out();
        }
        if (resolve_error) {
            throw db_error(db_error::code_type::resolve_failed, driver_type::driver,
                std::string("resolving ").append(driver_type::name).append(" host failed: ").append(error_code_text(resolve_error)),
                resolve_error.value());
        }
        co_return collect_db_resolved_addresses(completion, driver_type::driver, resource_);
    } catch (...) {
        (void)leases_.clear_deadline(index);
        throw;
    }
}

template <typename driver_type>
db_pool<driver_type>::slot_wait::slot_wait(db_pool& pool, std::size_t index, db_deadline_kind kind,
    db_slot_socket::native_socket_type native, db_socket_readiness wanted,
    std::optional<std::chrono::steady_clock::time_point> wait_deadline, const operation_timeout& operation_deadline) noexcept
    : pool_(pool),
      index_(index),
      kind_(kind),
      native_(native),
      wait_deadline_(wait_deadline),
      operation_deadline_(operation_deadline),
      socket_wait_(pool.slots_[index].wait_socket_.wait(wanted)) {}

template <typename driver_type>
db_pool<driver_type>::slot_wait::~slot_wait() {
    if (borrowed_) {
        pool_.slots_[index_].wait_active_ = false;
    }
}

template <typename driver_type>
bool db_pool<driver_type>::slot_wait::await_ready() {
    auto& leases = pool_.leases_;
    leases.throw_if_aborted(index_);
    auto& slot = pool_.slots_[index_];
    if (kind_ == db_deadline_kind::socket) {
        if (const auto error = slot.wait_socket_.attach(native_); error) {
            throw db_error(db_error::code_type::io_error, driver_type::driver,
                std::string("registering ").append(driver_type::name).append(" connection socket: ").append(error_code_text(error)),
                error.value());
        }
    }
    if (!leases.arm_deadline(index_, wait_deadline_, kind_)) {
        return true;
    }
    if (slot.wait_active_) {
        std::terminate();
    }
    slot.wait_active_ = true;
    borrowed_ = true;
    return kind_ == db_deadline_kind::sleep && leases.poll_deadline(index_);
}

template <typename driver_type>
bool db_pool<driver_type>::slot_wait::await_suspend(std::coroutine_handle<> handle) noexcept {
    if (kind_ == db_deadline_kind::sleep) {
        pool_.slots_[index_].deadline_continuation_ = handle;
        return true;
    }
    return socket_wait_.await_suspend(handle);
}

template <typename driver_type>
db_socket_wait_result db_pool<driver_type>::slot_wait::await_resume() {
    auto& leases = pool_.leases_;
    auto& slot = pool_.slots_[index_];
    db_socket_wait_result result_value;
    std::exception_ptr wait_failure;
    if (borrowed_) {
        if (kind_ == db_deadline_kind::socket) {
            try {
                result_value = socket_wait_.await_resume();
            } catch (...) {
                wait_failure = std::current_exception();
            }
        }
        slot.wait_active_ = false;
        borrowed_ = false;
    }
    if (kind_ == db_deadline_kind::sleep) {
        slot.deadline_continuation_ = {};
        result_value.expired_ = true;
    } else {
        // While connecting, the driver may close this socket and open its next
        // attempt's socket under the same handle value, which attach() could not
        // tell apart, so the duplicate must not outlive the wait. Once connected,
        // the driver keeps its socket until it drops it on a transport failure --
        // after which it reports no socket at all -- and never reconnects, so the
        // duplicate stays registered until the slot closes.
        if (!slot.connected_) {
            slot.wait_socket_.close();
        }
        result_value.expired_ = leases.poll_deadline(index_) ||
                                (wait_deadline_.has_value() && *wait_deadline_ <= std::chrono::steady_clock::now());
    }
    (void)leases.clear_deadline(index_);
    leases.throw_if_aborted(index_);
    if (slot.close_requested_) {
        throw pool_.closing_error();
    }
    if (operation_deadline_.expired()) {
        throw pool_.timed_out();
    }
    if (wait_failure != nullptr) {
        throw db_wait_failure(driver_type::driver, std::move(wait_failure));
    }
    return result_value;
}

// A failed transaction gives its slot back. When the connection is still
// usable the open transaction is rolled back first so the next caller does
// not inherit it; a rollback that fails closes the connection, which ends the
// transaction as well. The original failure is what the caller sees.
template <typename driver_type>
task<void> db_pool<driver_type>::retire_failed_transaction_slot(std::size_t index,
    std::pmr::memory_resource* resource, const operation_timeout& timeout, const std::exception_ptr& failure) {
    if (slots_[index].connected_ && db_slot_reusable_after(failure)) {
        try {
            co_await execute_control(index, "ROLLBACK", resource, timeout);
            co_return;
        } catch (...) {
        }
    }
    close_slot(index);
}

// A statement inside an open transaction runs on the slot the transaction
// already holds, so it neither acquires nor releases one -- unless it fails:
// a transaction whose statement failed cannot continue, so it is retired and
// the slot given back.
template <typename driver_type>
template <typename result_type>
task<result_type> db_pool<driver_type>::run_on_transaction_slot(std::size_t index, std::pmr::string sql,
    std::pmr::vector<db_value> params, std::pmr::memory_resource* resource, const operation_options& options,
    statement_hook_type<result_type> statement) {
    if (index >= slots_.size()) {
        throw std::logic_error("database transaction slot is invalid");
    }
    const operation_timeout timeout(options.timeout_);
    typename leases_type::cancellation_guard cancellation(leases_, index, options.stop_token_);
    std::exception_ptr failure;
    try {
        leases_.throw_if_aborted(index);
        if (!slots_[index].connected_) {
            co_await connect_slot(index, timeout);
        }
        co_return co_await statement(*this, index, sql, std::span<const db_value>(params), resource,
            timeout.constrained_by(config_.query_timeout_));
    } catch (...) {
        failure = std::current_exception();
    }
    co_await retire_failed_transaction_slot(index, resource, timeout, failure);
    cancellation.finish();
    release_slot(index);
    std::rethrow_exception(failure);
}

template <typename driver_type>
task<db_rows> db_pool<driver_type>::query(std::pmr::string sql, std::pmr::vector<db_value> params,
    std::pmr::memory_resource* resource, operation_options options, operation_scope* scope) {
    return driver_type::query_pipelined(*this, std::move(sql), std::move(params), resource, std::move(options), scope);
}

template <typename driver_type>
task<db_exec_result> db_pool<driver_type>::execute(std::pmr::string sql, std::pmr::vector<db_value> params,
    std::pmr::memory_resource* resource, operation_options options, operation_scope* scope) {
    return driver_type::execute_pipelined(*this, std::move(sql), std::move(params), resource, std::move(options), scope);
}

template <typename driver_type>
task<db_rows> db_pool<driver_type>::query_on_transaction_slot(std::size_t slot, std::pmr::string sql,
    std::pmr::vector<db_value> params, std::pmr::memory_resource* resource, const operation_options& options) {
    return run_on_transaction_slot<db_rows>(
        slot, std::move(sql), std::move(params), resource, options, &driver_type::query);
}

template <typename driver_type>
task<db_exec_result> db_pool<driver_type>::execute_on_transaction_slot(std::size_t slot, std::pmr::string sql,
    std::pmr::vector<db_value> params, std::pmr::memory_resource* resource, const operation_options& options) {
    return run_on_transaction_slot<db_exec_result>(
        slot, std::move(sql), std::move(params), resource, options, &driver_type::execute);
}

template <typename driver_type>
task<void> db_pool<driver_type>::execute_control(std::size_t index, std::string_view sql,
    std::pmr::memory_resource* resource, const operation_timeout& timeout) {
    leases_.throw_if_aborted(index);
    if (!slots_[index].connected_) {
        co_await connect_slot(index, timeout);
    }
    (void)co_await driver_type::execute(
        *this, index, sql, {}, resource, timeout.constrained_by(config_.query_timeout_));
}

// Starting a transaction transfers the acquired lease to the transaction
// object; any failure before that closes the connection and gives it back.
template <typename driver_type>
task<db_transaction> db_pool<driver_type>::begin_transaction(std::pmr::memory_resource* resource,
    operation_options operation_options_value, db_transaction_options transaction_options) {
    const auto plan = make_db_transaction_start_plan(driver_type::driver, transaction_options);
    const operation_timeout timeout(operation_options_value.timeout_);
    const auto index = co_await lease_acquire(*this, timeout, operation_options_value.stop_token_);
    typename leases_type::cancellation_guard cancellation(leases_, index, operation_options_value.stop_token_);
    try {
        if (!plan.configure_.empty()) {
            co_await execute_control(index, plan.configure_, resource, timeout);
        }
        co_await execute_control(index, plan.begin_, resource, timeout);
        co_return db_transaction(db_pool_ref_type{this}, index, resource, std::move(operation_options_value));
    } catch (...) {
        close_slot(index);
        cancellation.finish();
        release_slot(index);
        throw;
    }
}

// Ending a transaction runs the control statement on the slot the transaction
// holds and releases that slot exactly once. A COMMIT the server rejected has
// rolled the transaction back on a connection that is still usable; it is
// retired like a failed statement so the slot is clean for the next caller.
template <typename driver_type>
task<void> db_pool<driver_type>::finish_transaction(std::size_t index, std::string_view command,
    std::pmr::memory_resource* resource, const operation_options& options) {
    if (index >= slots_.size()) {
        throw std::logic_error("database transaction slot is invalid");
    }
    const operation_timeout timeout(options.timeout_);
    typename leases_type::cancellation_guard cancellation(leases_, index, options.stop_token_);
    std::exception_ptr failure;
    try {
        co_await execute_control(index, command, resource, timeout);
    } catch (...) {
        failure = std::current_exception();
    }
    if (failure != nullptr) {
        co_await retire_failed_transaction_slot(index, resource, timeout, failure);
    }
    cancellation.finish();
    release_slot(index);
    if (failure != nullptr) {
        std::rethrow_exception(failure);
    }
}

template <typename driver_type>
task<void> db_pool<driver_type>::commit_transaction(
    std::size_t slot, std::pmr::memory_resource* resource, const operation_options& options) {
    return finish_transaction(slot, "COMMIT", resource, options);
}

template <typename driver_type>
task<void> db_pool<driver_type>::rollback_transaction(
    std::size_t slot, std::pmr::memory_resource* resource, const operation_options& options) {
    return finish_transaction(slot, "ROLLBACK", resource, options);
}

template <typename driver_type>
void db_pool<driver_type>::abort_transaction(std::size_t slot) noexcept {
    if (slot >= slots_.size()) {
        return;
    }
    close_slot(slot);
    release_slot(slot);
}

// An unbuffered statement holds its slot until the stream result ends. A
// statement without a row set is refused after the driver has read its
// status, so its slot is clean and goes back without closing.
template <typename driver_type>
task<db_stream_result> db_pool<driver_type>::stream(std::pmr::string sql, std::pmr::vector<db_value> params,
    std::pmr::memory_resource* resource, operation_options options) {
    if (sql.empty()) {
        throw std::invalid_argument("SQL must not be empty");
    }
    const operation_timeout timeout(options.timeout_);
    const auto index = co_await lease_acquire(*this, timeout, options.stop_token_);
    typename leases_type::cancellation_guard cancellation(leases_, index, options.stop_token_);
    void* pending_result = nullptr;
    bool released = false;
    try {
        leases_.throw_if_aborted(index);
        if (!slots_[index].connected_) {
            co_await connect_slot(index, timeout);
        }
        auto start = co_await driver_type::start_stream(*this, index, sql, std::span<const db_value>(params),
            resource, timeout.constrained_by(config_.query_timeout_));
        if (!start.returns_rows_) {
            released = true;
            cancellation.finish();
            release_slot(index);
            throw std::invalid_argument("query_stream() requires row-producing SQL");
        }
        pending_result = start.result_;
        db_stream_result stream_result(
            db_pool_ref_type{this}, index, pending_result, std::move(start.column_names_), resource, std::move(options));
        // Ownership now belongs to the stream result.
        pending_result = nullptr;
        co_return stream_result;
    } catch (...) {
        if (!released) {
            driver_type::discard_stream(pending_result);
            close_slot(index);
            cancellation.finish();
            release_slot(index);
        }
        throw;
    }
}

template <typename driver_type>
task<std::optional<db_row>> db_pool<driver_type>::read_stream_row(std::size_t slot, void* result,
    std::pmr::vector<db_field>& fields, std::pmr::vector<std::pmr::string>& column_names,
    std::pmr::memory_resource* resource, const operation_options& options) {
    if (slot >= slots_.size()) {
        co_return std::nullopt;
    }
    co_return co_await driver_type::read_stream_row(*this, slot, result, fields, column_names, resource, options);
}

template <typename driver_type>
task<void> db_pool<driver_type>::close_stream(
    std::size_t slot, void* result, std::pmr::memory_resource* resource, const operation_options& options) {
    if (slot >= slots_.size()) {
        co_return;
    }
    co_await driver_type::close_stream(*this, slot, result, resource, options);
}

template <typename driver_type>
void db_pool<driver_type>::abort_stream(std::size_t slot, void* result) noexcept {
    if (slot >= slots_.size()) {
        return;
    }
    driver_type::discard_stream(result);
    close_slot(slot);
    release_slot(slot);
}

#ifdef RUVIA_ENABLE_MARIADB
template class db_pool<mariadb_driver>;
#endif
#ifdef RUVIA_ENABLE_POSTGRESQL
template class db_pool<postgresql_driver>;
#endif

}  // namespace ruvia::detail
