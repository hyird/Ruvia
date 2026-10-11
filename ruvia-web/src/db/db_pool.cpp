#include "db/db_pool.h"

#include <array>
#include <charconv>
#include <exception>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#include "ruvia/core/async.h"

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
        return db_error(db_error::code_type::io_error, driver, error.what(), error.code().value());
    } catch (const std::exception& error) {
        return db_error(db_error::code_type::io_error, driver, error.what());
    }
}

}  // namespace

std::array<char, 6> format_db_port(std::uint16_t port) {
    std::array<char, 6> output{};
    const auto parsed_value = std::to_chars(output.data(), output.data() + output.size() - 1, port);
    if (parsed_value.ec != std::errc{}) {
        throw std::logic_error("failed to format database port");
    }
    *parsed_value.ptr = '\0';
    return output;
}

template <typename driver_type>
void db_pool<driver_type>::slot_policy::throw_acquire_failure(
    const db_pool& pool, const pool_waiter_result& result) {
    if (result.timed_out() != nullptr) {
        throw db_error(db_error::code_type::timeout, pool.config_.driver_,
            "database connection pool acquire timed out");
    }
    if (result.cancelled() != nullptr) {
        throw db_error(db_error::code_type::cancelled, pool.config_.driver_, "database operation cancelled");
    }
    if (result.closed() != nullptr) {
        throw pool.closing_error();
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
      leases_(*this, db_connections_per_worker, worker_, resource_) {
    if (config_.driver_ != driver_type::driver) {
        throw std::invalid_argument(std::string(driver_type::name).append(" pool requires its own driver"));
    }
    slots_.reserve(db_connections_per_worker);
    for (std::size_t index = 0; index < db_connections_per_worker; ++index) {
        slots_.emplace_back(io_context_);
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
    for (std::size_t index = 0; index < slots_.size(); ++index) {
        close_slot(index);
    }
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

template <typename driver_type>
task<void> db_pool<driver_type>::ready_slot(std::size_t index, const operation_timeout& timeout) {
    leases_.throw_if_aborted(index);
    if (!slots_[index].connected_) {
        co_await connect_slot(index, timeout);
    }
}

// Resolving runs under the operation deadline with the slot's resolve deadline
// armed around it, so a stalled resolve is torn down with everything else on
// that slot. The deadline is checked once before arming and once after
// resuming, because the timer may have fired while this coroutine was
// suspended.
template <typename driver_type>
task<db_resolved_addresses_type> db_pool<driver_type>::resolve_host(
    std::size_t index, const operation_timeout& deadline_value) {
    const auto resolve_timed_out = [] {
        return db_error(db_error::code_type::timeout, driver_type::driver,
            std::string(driver_type::name).append(" host resolve timed out"));
    };

    leases_.throw_if_aborted(index);
    if (!leases_.arm_deadline(index, deadline_value.remaining(), db_deadline_kind::resolve)) {
        throw resolve_timed_out();
    }
    auto& slot = slots_[index];
    try {
        db_slot_active_wait_guard active_resolve(slot);
        const auto port = format_db_port(config_.port_);
        auto completion = co_await ruvia::async_asio<asio::ip::tcp::resolver::results_type>(
            [this, &slot, &port](auto handler) mutable {
                slot.resolver_.async_resolve(config_.host_, std::string_view(port.data()), std::move(handler));
            });
        const auto resolve_error = completion.error_code();
        auto results = std::move(completion).take_result();
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
                std::system_error(resolve_error,
                    std::string("resolving ").append(driver_type::name).append(" host failed"))
                    .what(),
                resolve_error.value());
        }
        co_return collect_db_resolved_addresses(results, driver_type::driver, resource_);
    } catch (...) {
        (void)leases_.clear_deadline(index);
        throw;
    }
}

template <typename driver_type>
task<db_socket_wait_result> db_pool<driver_type>::wait_socket(std::size_t index,
    db_slot_socket::native_socket_type native, db_socket_readiness wanted,
    std::optional<std::chrono::milliseconds> wait_timeout, const operation_timeout& operation_deadline) {
    leases_.throw_if_aborted(index);
    auto& slot = slots_[index];
    if (const auto error = slot.wait_socket_.attach(native); error) {
        throw db_error(db_error::code_type::io_error, driver_type::driver,
            std::string("registering ").append(driver_type::name).append(" connection socket: ").append(error.message()),
            error.value());
    }

    db_socket_wait_result result_value;
    std::exception_ptr wait_failure;
    if (leases_.arm_deadline(index, wait_timeout, db_deadline_kind::socket)) {
        db_slot_active_wait_guard active_wait(slot);
        try {
            result_value = co_await slot.wait_socket_.wait(wanted);
        } catch (...) {
            wait_failure = std::current_exception();
        }
    }
    // While connecting, the driver may close this socket and open its next
    // attempt's socket under the same handle value, which attach() could not
    // tell apart, so the duplicate must not outlive the wait. Once connected,
    // the driver keeps its socket until it drops it on a transport failure --
    // after which it reports no socket at all -- and never reconnects, so the
    // duplicate stays registered until the slot closes.
    if (!slot.connected_) {
        slot.wait_socket_.close();
    }
    result_value.expired_ = leases_.poll_deadline(index) ||
                            (wait_timeout.has_value() && wait_timeout->count() <= 0);
    (void)leases_.clear_deadline(index);
    leases_.throw_if_aborted(index);
    if (slot.close_requested_) {
        throw closing_error();
    }
    if (operation_deadline.expired()) {
        throw timed_out();
    }
    if (wait_failure != nullptr) {
        throw db_wait_failure(driver_type::driver, std::move(wait_failure));
    }
    co_return result_value;
}

// One buffered statement on a pooled connection: refuse empty SQL before
// taking a slot, hold the slot for the duration, and close the connection if
// the statement throws -- a slot whose statement failed mid-protocol cannot be
// reused. The lease guard releases the slot either way.
template <typename driver_type>
template <typename result_type>
task<result_type> db_pool<driver_type>::run_leased(std::pmr::string sql, std::pmr::vector<db_value> params,
    std::pmr::memory_resource* resource, operation_options options, statement_hook_type<result_type> statement) {
    if (sql.empty()) {
        throw std::invalid_argument("SQL must not be empty");
    }
    const operation_timeout timeout(options.timeout_);
    const auto index = co_await leases_.acquire(timeout, config_.acquire_timeout_, options.stop_token_);
    typename leases_type::lease_guard lease(leases_, index);
    typename leases_type::cancellation_guard cancellation(leases_, index, options.stop_token_);
    try {
        co_await ready_slot(index, timeout);
        co_return co_await statement(*this, index, sql, std::span<const db_value>(params), resource,
            timeout.constrained_by(config_.query_timeout_));
    } catch (...) {
        close_slot(index);
        throw;
    }
}

// A statement inside an open transaction runs on the slot the transaction
// already holds, so it neither acquires nor releases one -- unless it fails:
// a transaction whose statement failed mid-protocol cannot continue, so the
// connection is closed and the slot given back.
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
    try {
        co_await ready_slot(index, timeout);
        co_return co_await statement(*this, index, sql, std::span<const db_value>(params), resource,
            timeout.constrained_by(config_.query_timeout_));
    } catch (...) {
        close_slot(index);
        cancellation.finish();
        leases_.release(index);
        throw;
    }
}

template <typename driver_type>
task<db_rows> db_pool<driver_type>::query(std::pmr::string sql, std::pmr::vector<db_value> params,
    std::pmr::memory_resource* resource, operation_options options) {
    return run_leased<db_rows>(std::move(sql), std::move(params), resource, std::move(options), &driver_type::query);
}

template <typename driver_type>
task<db_exec_result> db_pool<driver_type>::execute(std::pmr::string sql, std::pmr::vector<db_value> params,
    std::pmr::memory_resource* resource, operation_options options) {
    return run_leased<db_exec_result>(
        std::move(sql), std::move(params), resource, std::move(options), &driver_type::execute);
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
    co_await ready_slot(index, timeout);
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
    const auto index =
        co_await leases_.acquire(timeout, config_.acquire_timeout_, operation_options_value.stop_token_);
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
        leases_.release(index);
        throw;
    }
}

// Ending a transaction runs the control statement on the slot the transaction
// holds and releases that slot exactly once. A failure closes the connection
// first: a slot whose COMMIT or ROLLBACK did not complete cannot be handed to
// the next caller.
template <typename driver_type>
task<void> db_pool<driver_type>::finish_transaction(std::size_t index, std::string_view command,
    std::pmr::memory_resource* resource, const operation_options& options) {
    if (index >= slots_.size()) {
        throw std::logic_error("database transaction slot is invalid");
    }
    const operation_timeout timeout(options.timeout_);
    typename leases_type::cancellation_guard cancellation(leases_, index, options.stop_token_);
    try {
        co_await execute_control(index, command, resource, timeout);
    } catch (...) {
        close_slot(index);
        cancellation.finish();
        leases_.release(index);
        throw;
    }
    cancellation.finish();
    leases_.release(index);
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
    leases_.release(slot);
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
    const auto index = co_await leases_.acquire(timeout, config_.acquire_timeout_, options.stop_token_);
    typename leases_type::cancellation_guard cancellation(leases_, index, options.stop_token_);
    void* pending_result = nullptr;
    bool released = false;
    try {
        co_await ready_slot(index, timeout);
        const auto start = co_await driver_type::start_stream(*this, index, sql, std::span<const db_value>(params),
            resource, timeout.constrained_by(config_.query_timeout_));
        if (!start.returns_rows_) {
            released = true;
            cancellation.finish();
            leases_.release(index);
            throw std::invalid_argument("query_stream() requires row-producing SQL");
        }
        pending_result = start.result_;
        db_stream_result stream_result(db_pool_ref_type{this}, index, pending_result, resource, std::move(options));
        // Ownership now belongs to the stream result.
        pending_result = nullptr;
        co_return stream_result;
    } catch (...) {
        if (!released) {
            driver_type::discard_stream(pending_result);
            close_slot(index);
            cancellation.finish();
            leases_.release(index);
        }
        throw;
    }
}

template <typename driver_type>
task<std::optional<db_row>> db_pool<driver_type>::read_stream_row(std::size_t slot, void* result,
    std::pmr::memory_resource* resource, const operation_options& options) {
    if (slot >= slots_.size()) {
        co_return std::nullopt;
    }
    co_return co_await driver_type::read_stream_row(*this, slot, result, resource, options);
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
    leases_.release(slot);
}

#ifdef RUVIA_ENABLE_MARIADB
template class db_pool<mariadb_driver>;
#endif
#ifdef RUVIA_ENABLE_POSTGRESQL
template class db_pool<postgresql_driver>;
#endif

}  // namespace ruvia::detail
