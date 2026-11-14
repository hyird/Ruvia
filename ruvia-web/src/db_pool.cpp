#include "db_pool.h"

#include <algorithm>
#include <array>
#include <exception>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <variant>

#include "borrowed_text.h"
#include "event_loop.h"

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#endif

#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

#include "connection_lifecycle.h"
#include "db.h"
#include "db_session.h"
#include "http_client_pool.h"
#include "http_header.h"
#include "redis.h"
#include "redis_pool.h"
#include "startup_config.h"
#include "stop_token.h"
#include "task_scope.h"

namespace ruvia::detail {
namespace {

void append_list_separator(std::string& output) {
    if (!output.empty()) {
        output.push_back(',');
    }
}

void append_db_resolved_address(db_resolved_addresses_type& addresses, const asio::ip::address& endpoint, db_driver driver) {
    std::string address;
    try {
        address = endpoint.to_string();
    } catch (const std::system_error& error) {
        throw db_error(db_error::code_type::resolve_failed, driver, system_error_text(error), error.code().value());
    }
    if (std::ranges::none_of(addresses, [&address](const std::string& existing) {
        return std::string_view(existing) == address;
    })) {
        addresses.emplace_back(address.data(), address.size());
    }
}

[[nodiscard]] const worker_ref& require_db_worker(const worker_ref& worker_value) {
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

// Both views refer to static SQL literals. The default transaction path keeps
// the same single control operation and allocations as an unconfigured BEGIN.
struct db_transaction_start_plan final {
    std::string_view configure_{};
    std::string_view begin_{};
};

[[nodiscard]] db_transaction_start_plan make_db_transaction_start_plan(db_driver driver, db_transaction_options options) {
    const auto isolation = static_cast<unsigned>(options.isolation_);
    const auto access = static_cast<unsigned>(options.access_mode_);
    if (isolation > static_cast<unsigned>(db_transaction_isolation::serializable) || access > static_cast<unsigned>(db_transaction_access_mode::read_only)) {
        throw std::invalid_argument("unsupported transaction options");
    }
    if (driver == db_driver::postgresql) {
        static constexpr std::array<std::array<std::string_view, 3>, 5> commands{{
            {"BEGIN", "BEGIN READ WRITE", "BEGIN READ ONLY"},
            {"BEGIN ISOLATION LEVEL READ UNCOMMITTED", "BEGIN ISOLATION LEVEL READ UNCOMMITTED READ WRITE", "BEGIN ISOLATION LEVEL READ UNCOMMITTED READ ONLY"},
            {"BEGIN ISOLATION LEVEL READ COMMITTED", "BEGIN ISOLATION LEVEL READ COMMITTED READ WRITE", "BEGIN ISOLATION LEVEL READ COMMITTED READ ONLY"},
            {"BEGIN ISOLATION LEVEL REPEATABLE READ", "BEGIN ISOLATION LEVEL REPEATABLE READ READ WRITE", "BEGIN ISOLATION LEVEL REPEATABLE READ READ ONLY"},
            {"BEGIN ISOLATION LEVEL SERIALIZABLE", "BEGIN ISOLATION LEVEL SERIALIZABLE READ WRITE", "BEGIN ISOLATION LEVEL SERIALIZABLE READ ONLY"},
        }};
        return {.begin_ = commands[isolation][access]};
    }
    if (driver == db_driver::mariadb) {
        static constexpr std::array<std::string_view, 5> configure{"", "SET TRANSACTION ISOLATION LEVEL READ UNCOMMITTED", "SET TRANSACTION ISOLATION LEVEL READ COMMITTED", "SET TRANSACTION ISOLATION LEVEL REPEATABLE READ", "SET TRANSACTION ISOLATION LEVEL SERIALIZABLE"};
        static constexpr std::array<std::string_view, 3> commands{"START TRANSACTION", "START TRANSACTION READ WRITE", "START TRANSACTION READ ONLY"};
        return {.configure_ = configure[isolation], .begin_ = commands[access]};
    }
    throw std::invalid_argument("transaction options require a database driver");
}

}  // namespace

db_resolved_addresses_type collect_db_resolved_addresses(const client_resolve_result& results, db_driver driver) {
    db_resolved_addresses_type addresses;
    if (results.literal_) {
        addresses.reserve(1);
        append_db_resolved_address(addresses, results.literal_->address(), driver);
    } else {
        addresses.reserve(results.addresses_.size());
        for (const auto& result : results.addresses_) {
            append_db_resolved_address(addresses, result.endpoint().address(), driver);
        }
    }
    if (addresses.empty()) {
        throw db_error(db_error::code_type::resolve_failed, driver, "database host resolved to no addresses");
    }
    return addresses;
}

std::string make_mariadb_resolved_host_list(std::span<const std::string> addresses) {
    if (addresses.empty()) {
        throw std::invalid_argument("MariaDB resolved host list must not be empty");
    }
    std::size_t total_size = addresses.size() - 1U;
    const bool multiple = addresses.size() > 1;
    for (const auto& address : addresses) {
        total_size += address.size() + (multiple && address.find(':') != std::string_view::npos ? 2U : 0U);
    }
    std::string output;
    output.reserve(total_size);
    for (const auto& address : addresses) {
        append_list_separator(output);
        if (multiple && (address.find(':') != std::string_view::npos)) {
            output.push_back('[');
            output.append(address);
            output.push_back(']');
        } else {
            output.append(address);
        }
    }
    return output;
}

postgresql_resolved_host_list make_postgresql_resolved_host_list(std::string_view logical_host, std::span<const std::string> addresses) {
    if (logical_host.empty() || addresses.empty()) {
        throw std::invalid_argument("PostgreSQL resolved host list must not be empty");
    }
    std::size_t addresses_size = addresses.size() - 1U;
    for (const auto& address : addresses) {
        addresses_size += address.size();
    }
    postgresql_resolved_host_list output{std::string{}, std::string{}};
    output.hosts_.reserve(addresses.size() * (logical_host.size() + 1U) - 1U);
    output.addresses_.reserve(addresses_size);
    for (const auto& address : addresses) {
        append_list_separator(output.hosts_);
        append_list_separator(output.addresses_);
        output.hosts_.append(logical_host);
        output.addresses_.append(address);
    }
    return output;
}

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
            throw db_error(db_error::code_type::timeout, pool.config_.driver_, "database connection pool acquire timed out");
        case worker_wait_status::cancelled:
            throw db_error(db_error::code_type::cancelled, pool.config_.driver_, "database operation cancelled");
        case worker_wait_status::closed:
        case worker_wait_status::worker_stopping:
            throw pool.closing_error();
        case worker_wait_status::value:
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
    auto& slot = *pool.slots_[index];
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
db_pool<driver_type>::db_pool(asio::io_context& io_context, const worker_ref& worker_value, db_config_storage config) :
    io_context_(io_context),
    config_(std::move(config)),
    worker_(require_db_worker(worker_value)),
    slots_(),
    leases_(*this, config_.pool_size_per_worker_, worker_),
    state_(*this, worker_) {
    if (config_.driver_ != driver_type::driver) {
        throw std::invalid_argument(std::string(driver_type::name).append(" pool requires its own driver"));
    }
    slots_.reserve(config_.pool_size_per_worker_);
    for (std::size_t index = 0; index < config_.pool_size_per_worker_; ++index) {
        slots_.push_back(std::make_unique<slot_type>(io_context_, worker_));
    }
}

template <typename driver_type>
db_pool<driver_type>::~db_pool() {
    close_now();
}

template <typename driver_type>
db_error db_pool<driver_type>::timed_out() const {
    return db_error(db_error::code_type::timeout, driver_type::driver, std::string(driver_type::name).append(" operation timed out"));
}

template <typename driver_type>
db_error db_pool<driver_type>::closing_error() const {
    return db_error(db_error::code_type::closing, driver_type::driver, "database client is closing");
}

template <typename driver_type>
task<void> db_pool<driver_type>::connect() {
    const operation_deadline timeout(std::nullopt);
    for (std::size_t index = 0; index < slots_.size(); ++index) {
        co_await open_slot(index, timeout);
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
    auto& released = slot(index);
    // Every path that ends a stream has freed or discarded its result by now.
    released.lease_timer_.cancel();
    released.lease_timeout_ = operation_deadline();
    released.lease_busy_ = false;
    released.stream_result_ = nullptr;
    // A drained command can still leave a transaction open or autocommit off,
    // including a rejected stream and COMMIT/ROLLBACK with chaining enabled.
    // Apply the same isolation boundary before either scheduler reassigns it.
    if (released.connected_ && !driver_type::reusable_connection(*released.connection_)) {
        close_slot(index);
    }
    if (!state_.take_released_slot(index)) {
        leases_.release(index);
    }
}

template <typename driver_type>
void db_pool<driver_type>::start_lease(std::size_t index) {
    auto& leased = slot(index);
    leased.lease_busy_ = true;
    leased.lease_timeout_ = operation_deadline(config_.transaction_timeout_);
    if (const auto deadline = leased.lease_timeout_.deadline(); deadline.has_value()) {
        // A stopping worker schedules nothing; its shutdown closes the pool,
        // and every operation stays bounded by the lease deadline meanwhile.
        (void)worker_.schedule_timer(leased.lease_timer_, *deadline, [this, index](worker_timer_outcome outcome) noexcept {
            if (outcome == worker_timer_outcome::expired) {
                expire_idle_lease(index);
            }
        });
    }
}

template <typename driver_type>
void db_pool<driver_type>::enter_lease_operation(std::size_t index) {
    leases_.throw_if_aborted(index);
    auto& leased = slot(index);
    // The lease timer may not have run yet; the caller's failure path closes
    // the connection either way.
    if (leased.lease_timeout_.expired()) {
        throw lease_timed_out();
    }
    leased.lease_busy_ = true;
}

template <typename driver_type>
void db_pool<driver_type>::leave_lease_operation(std::size_t index) {
    auto& leased = slot(index);
    leased.lease_busy_ = false;
    // The operation finished after its lease timer fired and left it alone:
    // fail it now, as no timer will close the idle connection later.
    if (leased.lease_timeout_.expired()) {
        throw lease_timed_out();
    }
}

template <typename driver_type>
operation_deadline db_pool<driver_type>::lease_operation_timeout(std::size_t index, const operation_deadline& timeout) const noexcept {
    const auto& lease = slots_[index]->lease_timeout_;
    const auto lease_deadline = lease.deadline();
    const auto own_deadline = timeout.deadline();
    if (lease_deadline.has_value() && (!own_deadline.has_value() || *lease_deadline < *own_deadline)) {
        return lease;
    }
    return timeout;
}

template <typename driver_type>
void db_pool<driver_type>::expire_idle_lease(std::size_t index) noexcept {
    auto& leased = slot(index);
    if (leased.lease_busy_) {
        return;
    }
    driver_type::discard_stream(std::exchange(leased.stream_result_, nullptr));
    close_slot(index);
}

template <typename driver_type>
db_error db_pool<driver_type>::lease_timed_out() const {
    return db_error(db_error::code_type::timeout, driver_type::driver, std::string(driver_type::name).append(" transaction or stream exceeded transaction_timeout"));
}

// Nothing has been sent on a fresh lease, so replacing the connection is
// always safe; a pool that does not reconnect then reports the ended session
// from connect_slot() before any statement.
template <typename driver_type>
void db_pool<driver_type>::retire_dead_idle_connection(std::size_t index) noexcept {
    auto& leased = slot(index);
    if (leased.connected_ && !driver_type::idle_connection_alive(*leased.connection_)) {
        close_slot(index);
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
db_pool<driver_type>::exclusive_wait::exclusive_wait(db_pool& pool, const stop_token& stop) noexcept :
    pool_(pool),
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
    auto& slot = *slots_[index];
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
task<void> db_pool<driver_type>::connect_slot(std::size_t index, const operation_deadline& timeout) {
    if (!config_.reconnect_ && !slot(index).connected_ && !leases_.closing()) {
        throw db_error(db_error::code_type::io_error, driver_type::driver, std::string(driver_type::name).append(" session ended, and this pool does not reconnect"));
    }
    co_await open_slot(index, timeout);
}

template <typename driver_type>
task<void> db_pool<driver_type>::open_slot(std::size_t index, const operation_deadline& timeout) {
    if (leases_.closing()) {
        throw closing_error();
    }
    auto& slot = *slots_[index];
    if (slot.connected_) {
        co_return;
    }
    leases_.throw_if_aborted(index);
    const operation_deadline deadline_value = timeout.constrained_by(config_.connect_timeout_);
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
task<db_resolved_addresses_type> db_pool<driver_type>::resolve_host(std::size_t index, const operation_deadline& deadline_value) {
    const auto resolve_timed_out = [] {
        return db_error(db_error::code_type::timeout, driver_type::driver, std::string(driver_type::name).append(" host resolve timed out"));
    };

    leases_.throw_if_aborted(index);
    if (!leases_.arm_deadline(index, deadline_value.deadline(), db_deadline_kind::resolve)) {
        throw resolve_timed_out();
    }
    auto& slot = *slots_[index];
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
            throw db_error(db_error::code_type::resolve_failed, driver_type::driver, std::string("resolving ").append(driver_type::name).append(" host failed: ").append(error_code_text(resolve_error)), resolve_error.value());
        }
        co_return collect_db_resolved_addresses(completion, driver_type::driver);
    } catch (...) {
        (void)leases_.clear_deadline(index);
        throw;
    }
}

template <typename driver_type>
db_pool<driver_type>::slot_wait::slot_wait(db_pool& pool, std::size_t index, db_deadline_kind kind, db_slot_socket::native_socket_type native, db_socket_readiness wanted, std::optional<std::chrono::steady_clock::time_point> wait_deadline, const operation_deadline& operation_deadline_value) noexcept :
    pool_(pool),
    index_(index),
    kind_(kind),
    native_(native),
    wait_deadline_(wait_deadline),
    operation_deadline_(operation_deadline_value),
    socket_wait_(pool.slots_[index]->wait_socket_.wait(wanted)) {}

template <typename driver_type>
db_pool<driver_type>::slot_wait::~slot_wait() {
    if (borrowed_) {
        pool_.slots_[index_]->wait_active_ = false;
    }
}

template <typename driver_type>
bool db_pool<driver_type>::slot_wait::await_ready() {
    auto& leases = pool_.leases_;
    leases.throw_if_aborted(index_);
    auto& slot = *pool_.slots_[index_];
    if (kind_ == db_deadline_kind::socket) {
        if (const auto error = slot.wait_socket_.attach(native_); error) {
            throw db_error(db_error::code_type::io_error, driver_type::driver, std::string("registering ").append(driver_type::name).append(" connection socket: ").append(error_code_text(error)), error.value());
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
        pool_.slots_[index_]->deadline_continuation_ = handle;
        return true;
    }
    return socket_wait_.await_suspend(handle);
}

template <typename driver_type>
db_socket_wait_result db_pool<driver_type>::slot_wait::await_resume() {
    auto& leases = pool_.leases_;
    auto& slot = *pool_.slots_[index_];
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
        result_value.expired_ = leases.poll_deadline(index_) || (wait_deadline_.has_value() && *wait_deadline_ <= std::chrono::steady_clock::now());
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
// Callers must also close and release the lease if creating this task fails.
template <typename driver_type>
task<void> db_pool<driver_type>::retire_failed_transaction_slot(std::size_t index, const operation_deadline& timeout, const std::exception_ptr& failure) {
    if (slot(index).connected_ && db_slot_reusable_after(failure)) {
        try {
            co_await execute_control(index, "ROLLBACK", timeout);
            co_return;
        } catch (...) {
        }
    }
    close_slot(index);
}

// A statement inside an open transaction runs on the slot the transaction
// already holds, so it neither acquires nor releases one -- unless it fails:
// a transaction whose statement failed cannot continue, so it is retired and
// the slot given back. The lease deadline fails it like any other failure.
template <typename driver_type>
template <typename result_type>
task<result_type> db_pool<driver_type>::run_on_transaction_slot(std::size_t index, std::string sql, std::vector<db_value> params, const operation_options& options, statement_hook_type<result_type> statement) {
    require_slot_index(index);
    const auto timeout = lease_operation_timeout(index, operation_deadline(options.timeout_));
    typename leases_type::cancellation_guard cancellation(leases_, index, options.stop_token_);
    std::exception_ptr failure;
    try {
        enter_lease_operation(index);
        if (!slot(index).connected_) {
            co_await connect_slot(index, timeout);
        }
        auto result_value = co_await statement(*this, index, sql, std::span<const db_value>(params), timeout.constrained_by(config_.query_timeout_));
        leave_lease_operation(index);
        co_return result_value;
    } catch (...) {
        failure = std::current_exception();
    }
    try {
        // Release the failed statement's storage before awaiting ROLLBACK.
        std::string(sql).swap(sql);
        std::vector<db_value>(params).swap(params);
        co_await retire_failed_transaction_slot(index, timeout, failure);
    } catch (...) {
        close_slot(index);
    }
    cancellation.finish();
    release_slot(index);
    std::rethrow_exception(failure);
}

template <typename driver_type>
task<db_rows> db_pool<driver_type>::query(std::string sql, std::vector<db_value> params, operation_options options, request_io_metrics* io_metrics) {
    return run_pipelined<typename driver_type::pipeline_io_type, db_rows>(*this, std::move(sql), std::move(params), std::move(options), io_metrics);
}

template <typename driver_type>
task<db_exec_result> db_pool<driver_type>::execute(std::string sql, std::vector<db_value> params, operation_options options, request_io_metrics* io_metrics) {
    return run_pipelined<typename driver_type::pipeline_io_type, db_exec_result>(*this, std::move(sql), std::move(params), std::move(options), io_metrics);
}

template <typename driver_type>
task<db_rows> db_pool<driver_type>::query_on_transaction_slot(std::size_t slot, std::string sql, std::vector<db_value> params, const operation_options& options) {
    return run_on_transaction_slot<db_rows>(slot, std::move(sql), std::move(params), options, &driver_type::query);
}

template <typename driver_type>
task<db_exec_result> db_pool<driver_type>::execute_on_transaction_slot(std::size_t slot, std::string sql, std::vector<db_value> params, const operation_options& options) {
    return run_on_transaction_slot<db_exec_result>(slot, std::move(sql), std::move(params), options, &driver_type::execute);
}

template <typename driver_type>
task<void> db_pool<driver_type>::execute_control(std::size_t index, std::string_view sql, const operation_deadline& timeout) {
    leases_.throw_if_aborted(index);
    if (!slot(index).connected_) {
        co_await connect_slot(index, timeout);
    }
    (void)co_await driver_type::execute(*this, index, sql, {}, timeout.constrained_by(config_.query_timeout_));
}

// Starting a transaction transfers the acquired lease to the transaction
// object; any failure before that closes the connection and gives it back.
// The lease deadline starts once the lease is held, so it bounds connecting
// and BEGIN but not the wait for a free connection.
template <typename driver_type>
task<db_transaction> db_pool<driver_type>::begin_transaction(operation_options operation_options_value, db_transaction_options transaction_options) {
    const auto plan = make_db_transaction_start_plan(driver_type::driver, transaction_options);
    const operation_deadline timeout(operation_options_value.timeout_);
    const auto index = co_await lease_acquire(*this, timeout, operation_options_value.stop_token_);
    typename leases_type::cancellation_guard cancellation(leases_, index, operation_options_value.stop_token_);
    try {
        start_lease(index);
        const auto start_timeout = lease_operation_timeout(index, timeout);
        if (!plan.configure_.empty()) {
            co_await execute_control(index, plan.configure_, start_timeout);
        }
        co_await execute_control(index, plan.begin_, start_timeout);
        leave_lease_operation(index);
        co_return db_transaction(db_pool_ref_type{this}, index, std::move(operation_options_value));
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
// After the lease deadline passed, nothing is sent: the connection is (or is
// now) closed, which already ended the transaction on the server.
template <typename driver_type>
task<void> db_pool<driver_type>::finish_transaction(std::size_t index, std::string_view command, const operation_options& options) {
    require_slot_index(index);
    const auto timeout = lease_operation_timeout(index, operation_deadline(options.timeout_));
    typename leases_type::cancellation_guard cancellation(leases_, index, options.stop_token_);
    std::exception_ptr failure;
    try {
        enter_lease_operation(index);
        co_await execute_control(index, command, timeout);
    } catch (...) {
        failure = std::current_exception();
    }
    if (failure != nullptr) {
        try {
            co_await retire_failed_transaction_slot(index, timeout, failure);
        } catch (...) {
            close_slot(index);
        }
    }
    cancellation.finish();
    release_slot(index);
    if (failure != nullptr) {
        std::rethrow_exception(failure);
    }
}

template <typename driver_type>
task<void> db_pool<driver_type>::commit_transaction(std::size_t slot, const operation_options& options) {
    return finish_transaction(slot, "COMMIT", options);
}

template <typename driver_type>
task<void> db_pool<driver_type>::rollback_transaction(std::size_t slot, const operation_options& options) {
    return finish_transaction(slot, "ROLLBACK", options);
}

template <typename driver_type>
void db_pool<driver_type>::abort_transaction(std::size_t slot) noexcept {
    require_slot_index(slot);
    close_slot(slot);
    release_slot(slot);
}

template <typename driver_type>
void db_pool<driver_type>::require_slot_index(std::size_t index) const noexcept {
    if (index >= slots_.size()) {
        std::terminate();
    }
}

template <typename driver_type>
void db_pool<driver_type>::fail_stream(std::size_t index, typename leases_type::cancellation_guard& cancellation) noexcept {
    driver_type::discard_stream(std::exchange(slot(index).stream_result_, nullptr));
    close_slot(index);
    cancellation.finish();
    release_slot(index);
}

// An unbuffered statement holds its slot until the stream result ends. A
// statement without a row set is refused after the driver has read its
// status, so its slot is clean and goes back without closing. The lease
// deadline starts once the lease is held.
template <typename driver_type>
task<db_stream_result> db_pool<driver_type>::stream(std::string sql, std::vector<db_value> params, operation_options options) {
    const operation_deadline timeout(options.timeout_);
    const auto index = co_await lease_acquire(*this, timeout, options.stop_token_);
    typename leases_type::cancellation_guard cancellation(leases_, index, options.stop_token_);
    bool released = false;
    try {
        leases_.throw_if_aborted(index);
        start_lease(index);
        const auto start_timeout = lease_operation_timeout(index, timeout);
        if (!slot(index).connected_) {
            co_await connect_slot(index, start_timeout);
        }
        auto start = co_await driver_type::start_stream(*this, index, sql, std::span<const db_value>(params), start_timeout.constrained_by(config_.query_timeout_));
        if (!start.returns_rows_) {
            released = true;
            cancellation.finish();
            release_slot(index);
            throw std::invalid_argument("query_stream() requires row-producing SQL");
        }
        slot(index).stream_result_ = start.result_;
        leave_lease_operation(index);
        co_return db_stream_result(db_pool_ref_type{this}, index, std::move(start.column_names_), std::move(options));
    } catch (...) {
        if (!released) {
            fail_stream(index, cancellation);
        }
        throw;
    }
}

// Each read runs under its own deadline, like any statement, bounded by the
// stream's lease deadline. The slot goes back exactly once: when the rows run
// out, or after a failure, which closes the connection with whatever it was
// still reading.
template <typename driver_type>
task<std::optional<db_row>> db_pool<driver_type>::read_stream_row(std::size_t index, std::vector<db_field>& fields, std::vector<std::string>& column_names, const operation_options& options) {
    require_slot_index(index);
    const auto timeout = lease_operation_timeout(index, operation_deadline(options.timeout_));
    typename leases_type::cancellation_guard cancellation(leases_, index, options.stop_token_);
    std::optional<db_stream_step> step;
    try {
        enter_lease_operation(index);
        step.emplace(co_await driver_type::read_stream_row(*this, index, slot(index).stream_result_, fields, column_names, timeout.constrained_by(config_.query_timeout_)));
        if (std::holds_alternative<db_row>(*step)) {
            leave_lease_operation(index);
        }
    } catch (...) {
        fail_stream(index, cancellation);
        throw;
    }
    if (auto* row = std::get_if<db_row>(&*step)) {
        co_return std::move(*row);
    }
    cancellation.finish();
    release_slot(index);
    if (std::get<db_stream_end>(*step) == db_stream_end::no_row_set) {
        throw std::invalid_argument("query_stream() requires row-producing SQL");
    }
    co_return std::nullopt;
}

template <typename driver_type>
task<void> db_pool<driver_type>::close_stream(std::size_t index, const operation_options& options) {
    require_slot_index(index);
    const auto timeout = lease_operation_timeout(index, operation_deadline(options.timeout_));
    typename leases_type::cancellation_guard cancellation(leases_, index, options.stop_token_);
    try {
        enter_lease_operation(index);
        co_await driver_type::close_stream(*this, index, slot(index).stream_result_, timeout.constrained_by(config_.query_timeout_));
    } catch (...) {
        fail_stream(index, cancellation);
        throw;
    }
    cancellation.finish();
    release_slot(index);
}

template <typename driver_type>
void db_pool<driver_type>::abort_stream(std::size_t index) noexcept {
    require_slot_index(index);
    driver_type::discard_stream(std::exchange(slot(index).stream_result_, nullptr));
    close_slot(index);
    release_slot(index);
}

#ifdef RUVIA_ENABLE_MARIADB
template class db_pool<mariadb_driver>;
#endif
#ifdef RUVIA_ENABLE_POSTGRESQL
template class db_pool<postgresql_driver>;
#endif

#if defined(_WIN32)
db_slot_socket::db_slot_socket(asio::io_context& io_context) :
    socket_(io_context) {}
#else
db_slot_socket::db_slot_socket(asio::io_context& io_context) :
    descriptor_(io_context) {}
#endif

db_slot_socket::waitable_type& db_slot_socket::waitable() noexcept {
#if defined(_WIN32)
    return socket_;
#else
    return descriptor_;
#endif
}

bool db_slot_socket::readiness_awaiter::await_suspend(std::coroutine_handle<> handle) noexcept {
    continuation_ = handle;
    try {
        // Count an operation only after Asio accepts it. If a later initiation
        // throws, cancel and stay suspended until the already-published
        // handlers have drained before surfacing the original exception.
        if (has_db_socket_readiness(wanted_, db_socket_readiness::read)) {
            start(waitable_type::wait_read, db_socket_readiness::read);
        }
        if (has_db_socket_readiness(wanted_, db_socket_readiness::write)) {
            start(waitable_type::wait_write, db_socket_readiness::write);
        }
        if (has_db_socket_readiness(wanted_, db_socket_readiness::error)) {
            start(waitable_type::wait_error, db_socket_readiness::error);
        }
    } catch (...) {
        initiation_failure_ = std::current_exception();
        completed_ = true;
        socket_.cancel();
    }
    return pending_ != 0;
}

void db_slot_socket::readiness_awaiter::start(waitable_type::wait_type wait_type, db_socket_readiness readiness) {
    socket_.waitable().async_wait(wait_type, [this, readiness](std::error_code wait_error) noexcept {
        complete(readiness, wait_error);
    });
    ++pending_;
}

void db_slot_socket::readiness_awaiter::complete(db_socket_readiness readiness, std::error_code error) noexcept {
    if (!completed_) {
        completed_ = true;
        result_.error_ = error;
        if (!error) {
            result_.ready_ = readiness;
        }
        if (pending_ > 1) {
            socket_.cancel();
        }
    }
    if (--pending_ == 0) {
        // Asio releases each wait operation before invoking its handler, so
        // once the last handler runs nothing still borrows this awaiter.
        continuation_.resume();
    }
}

db_socket_wait_result db_slot_socket::readiness_awaiter::await_resume() const {
    if (initiation_failure_ != nullptr) {
        std::rethrow_exception(initiation_failure_);
    }
    return result_;
}

std::error_code db_slot_socket::attach(native_socket_type driver_socket) noexcept {
    if (driver_socket == invalid_socket) {
        return std::make_error_code(std::errc::bad_file_descriptor);
    }
    if (source_ == driver_socket) {
        return {};
    }
    close();
    std::error_code ec;
#if defined(_WIN32)
    // WSADuplicateSocket is the documented way to duplicate a socket
    // (DuplicateHandle bypasses layered providers). Within one process the
    // result is a second handle to the same kernel socket object, so the IOCP
    // association made below covers the driver's handle too. The drivers only
    // issue synchronous, non-overlapped calls on it, which post no completion
    // packets.
    WSAPROTOCOL_INFOW protocol_info{};
    if (::WSADuplicateSocketW(static_cast<SOCKET>(driver_socket), ::GetCurrentProcessId(), &protocol_info) != 0) {
        return std::error_code(::WSAGetLastError(), std::system_category());
    }
    if (protocol_info.iSocketType != SOCK_STREAM || (protocol_info.iAddressFamily != AF_INET && protocol_info.iAddressFamily != AF_INET6)) {
        return std::make_error_code(std::errc::address_family_not_supported);
    }
    const SOCKET duplicate = ::WSASocketW(FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, &protocol_info, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
    if (duplicate == INVALID_SOCKET) {
        return std::error_code(::WSAGetLastError(), std::system_category());
    }
    socket_.assign(protocol_info.iAddressFamily == AF_INET ? asio::ip::tcp::v4() : asio::ip::tcp::v6(), duplicate, ec);
    if (ec) {
        (void)::closesocket(duplicate);
        return ec;
    }
#else
    const int duplicate = ::fcntl(driver_socket, F_DUPFD_CLOEXEC, 0);
    if (duplicate < 0) {
        return std::error_code(errno, std::system_category());
    }
    descriptor_.assign(duplicate, ec);
    if (ec) {
        (void)::close(duplicate);
        return ec;
    }
#endif
    source_ = driver_socket;
    return {};
}

void db_slot_socket::cancel() noexcept {
    std::error_code ignored;
    waitable().cancel(ignored);
}

void db_slot_socket::close() noexcept {
    std::error_code ec;
#if defined(_WIN32)
    // The duplicate is a second handle to the driver's socket object, and an
    // IOCP association belongs to that object rather than to a handle.
    // Closing only the duplicate would leave the driver's socket associated
    // with this io_context, and a later duplicate of the same socket could
    // not register. Dissociate first; if that fails, the plain close below
    // still runs and such a later registration reports the failure.
    if (socket_.is_open()) {
        const auto released = socket_.release(ec);
        if (!ec) {
            (void)::closesocket(released);
        }
    }
#endif
    // Asio invalidates its handle even when the close reports an error, and
    // the handle is ours alone, so there is nothing to retry.
    waitable().close(ec);
    source_ = invalid_socket;
}

std::error_code set_db_socket_non_blocking(db_slot_socket::native_socket_type driver_socket) noexcept {
#if defined(_WIN32)
    u_long non_blocking = 1;
    if (::ioctlsocket(static_cast<SOCKET>(driver_socket), FIONBIO, &non_blocking) != 0) {
        return std::error_code(::WSAGetLastError(), std::system_category());
    }
#else
    const int flags = ::fcntl(driver_socket, F_GETFL, 0);
    if (flags < 0 || ((flags & O_NONBLOCK) == 0 && ::fcntl(driver_socket, F_SETFL, flags | O_NONBLOCK) != 0)) {
        return std::error_code(errno, std::system_category());
    }
#endif
    return {};
}

db_idle_socket_status peek_db_idle_socket(db_slot_socket::native_socket_type driver_socket) noexcept {
    char byte{};
#if defined(_WIN32)
    const int received = ::recv(static_cast<SOCKET>(driver_socket), &byte, 1, MSG_PEEK);
    if (received == SOCKET_ERROR) {
        return ::WSAGetLastError() == WSAEWOULDBLOCK ? db_idle_socket_status::quiet : db_idle_socket_status::closed;
    }
#else
    ssize_t received = 0;
    do {
        received = ::recv(driver_socket, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
    } while (received < 0 && errno == EINTR);
    if (received < 0) {
        return errno == EAGAIN || errno == EWOULDBLOCK ? db_idle_socket_status::quiet : db_idle_socket_status::closed;
    }
#endif
    return received == 0 ? db_idle_socket_status::closed : db_idle_socket_status::pending_data;
}

}  // namespace ruvia::detail

#ifdef RUVIA_ENABLE_REDIS

namespace ruvia::detail {
namespace {
void number(std::string& out, std::uint64_t value) {
    std::array<char, 8> encoded;
    for (unsigned index = 0; index < encoded.size(); ++index) {
        encoded[index] = static_cast<char>((value >> (index * 8)) & 255);
    }
    out.append(encoded.data(), encoded.size());
}
void text(std::string& out, std::string_view value) {
    number(out, value.size());
    out.append(value);
}
[[noreturn]] void invalid_cache() {
    throw db_conversion_error(db_conversion_error::code_type::invalid_format, "invalid database query cache entry");
}
struct reader {
    std::string_view bytes_;
    std::uint64_t number() {
        if (bytes_.size() < 8) {
            invalid_cache();
        }
        std::uint64_t value = 0;
        for (unsigned index = 0; index < 8; ++index) {
            value |= std::uint64_t(static_cast<unsigned char>(bytes_[index])) << (index * 8);
        }
        bytes_.remove_prefix(8);
        return value;
    }
    std::string_view text() {
        const auto size = number();
        if (size > bytes_.size()) {
            invalid_cache();
        }
        auto result_value = bytes_.substr(0, static_cast<std::size_t>(size));
        bytes_.remove_prefix(static_cast<std::size_t>(size));
        return result_value;
    }
};

// Leads every entry, so a value another writer put under a cache key is
// refused before it is decoded.
constexpr std::string_view db_cache_entry_magic = "RUVIAQC";
constexpr std::string_view cache_prefix = "ruvia:qc:";
constexpr std::size_t digest_text_bytes = 2 * sha256_digest_size;

[[nodiscard]] std::string make_id_key_from_hex(std::string_view prefix, db_cache_shape shape, std::string_view digest_hex) {
    const auto shape_tag = db_cache_shape_tag(shape);
    std::string result;
    result.reserve(prefix.size() + 3 + shape_tag.size() + 1 + digest_hex.size());
    result.append(prefix).append("id:").append(shape_tag).push_back(':');
    result.append(digest_hex);
    return result;
}

[[nodiscard]] std::string make_id_key(std::string_view prefix, db_cache_shape shape, const sha256_digest& hash) {
    std::array<char, digest_text_bytes> hex{};
    http_encode_lower_hex(hex.data(), hash);
    return make_id_key_from_hex(prefix, shape, std::string_view(hex.data(), hex.size()));
}

void release_cache_storage(void* storage) noexcept {
    auto* const bytes = static_cast<std::string*>(storage);
    delete bytes;
}

// SQL and parameters can be large, so their byte encoding streams into the
// digest instead of being copied into a temporary string first.
void digest_text(sha256& digest, std::string_view value) {
    digest.update_u64_le(value.size());
    digest.update(value);
}

}  // namespace

task<db_rows> db_cache_query::operator()(operation_options options) && {
    return query_db_backend(backend_, std::move(sql_), std::move(params_), options);
}

// Format: the entry magic, row count, column count, the column names once, then for
// every row a row marker followed, for every field, by a presence byte and its
// text when present. All rows of one result share a single schema, so the
// names are not repeated per row. The marker gives every row, including a
// zero-column one, a serialized cost, so a decoder bounds the row count by the
// entry size before it allocates.
constexpr char db_cache_row_marker = '\2';

std::string encode_db_cache_rows(const db_rows& rows) {
    const auto names = rows.column_names();
    std::size_t total_bytes = db_cache_entry_magic.size() + 8U + 8U;
    for (const auto& name : names) {
        total_bytes += 8U + name.size();
    }
    for (const auto& row : rows) {
        if (row.size() != names.size()) {
            invalid_cache();
        }
        total_bytes += 1U + row.size();
        for (std::size_t index = 0; index < row.size(); ++index) {
            if (const auto value = row[index].value()) {
                total_bytes += 8U + value->size();
            }
        }
    }
    std::string out;
    out.reserve(total_bytes);
    out.append(db_cache_entry_magic);
    number(out, rows.size());
    number(out, names.size());
    for (const auto& name : names) {
        text(out, name);
    }
    for (const auto& row : rows) {
        out.push_back(db_cache_row_marker);
        for (std::size_t index = 0; index < row.size(); ++index) {
            const auto value = row[index].value();
            out.push_back(value ? '\1' : '\0');
            if (value) {
                text(out, *value);
            }
        }
    }
    return out;
}
db_rows decode_db_cache_rows(std::string bytes_value) {
    if (!bytes_value.starts_with(db_cache_entry_magic)) {
        invalid_cache();
    }
    reader reader_value{std::string_view(bytes_value).substr(db_cache_entry_magic.size())};
    const auto count = reader_value.number();
    const auto columns = reader_value.number();
    if (columns > reader_value.bytes_.size() / 8) {
        invalid_cache();
    }
    auto result_value = db_result_access::make_result();
    auto& names = db_result_access::column_names(result_value);
    names.reserve(static_cast<std::size_t>(columns));
    for (std::uint64_t index = 0; index < columns; ++index) {
        names.emplace_back(reader_value.text());
    }
    // Every row takes at least its marker and one presence byte per field,
    // which also bounds count * columns by the remaining entry size.
    if (count > reader_value.bytes_.size() / (columns + 1)) {
        invalid_cache();
    }
    // Only fields can borrow the encoded storage. Empty results and
    // zero-column rows need no backing allocation after decoding.
    if (count != 0 && columns != 0) {
        // Rebind to the result's resource when Redis and the database use
        // different worker pools.
        const auto consumed = bytes_value.size() - reader_value.bytes_.size();
        auto storage = std::make_unique<std::string>(std::move(bytes_value));
        reader_value.bytes_ = std::string_view(*storage).substr(consumed);
        db_result_access::own_raw_result(result_value, storage.release(), &release_cache_storage);
    }
    auto& fields_value = db_result_access::fields(result_value);
    auto& rows = db_result_access::rows(result_value);
    fields_value.reserve(static_cast<std::size_t>(count * columns));
    rows.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t row_index = 0; row_index < count; ++row_index) {
        if (reader_value.bytes_.empty() || reader_value.bytes_.front() != db_cache_row_marker) {
            invalid_cache();
        }
        reader_value.bytes_.remove_prefix(1);
        const auto row_start = fields_value.size();
        for (std::uint64_t index = 0; index < columns; ++index) {
            if (reader_value.bytes_.empty()) {
                invalid_cache();
            }
            const char present = reader_value.bytes_.front();
            reader_value.bytes_.remove_prefix(1);
            if (present == '\0') {
                fields_value.push_back(db_result_access::null_field());
            } else if (present == '\1') {
                fields_value.push_back(db_result_access::borrowed_field(reader_value.text()));
            } else {
                invalid_cache();
            }
        }
        // The field storage was reserved up front, so borrowed rows stay valid.
        rows.push_back(db_result_access::borrowed_row(fields_value.data() + row_start, static_cast<std::size_t>(columns), names.data(), names.size()));
    }
    if (!reader_value.bytes_.empty()) {
        invalid_cache();
    }
    return result_value;
}
std::string db_cache_prefix(std::string_view name_space, std::string_view alias, const db_config_storage& config) {
    const auto server_name = config.tls_.server_name_ ? std::string_view(*config.tls_.server_name_) : std::string_view{};
    sha256 identity;
    digest_text(identity, name_space);
    digest_text(identity, alias);
    identity.update_u64_le(static_cast<std::uint64_t>(config.driver_));
    digest_text(identity, config.host_);
    identity.update_u64_le(config.port_);
    digest_text(identity, config.database_);
    digest_text(identity, config.username_);
    digest_text(identity, server_name);
    std::string result_value;
    result_value.reserve(cache_prefix.size() + digest_text_bytes + 1);
    result_value.append(cache_prefix);
    http_append_lower_hex(result_value, identity.finish());
    result_value.push_back(':');
    return result_value;
}
void append_db_cache_id_keys(std::vector<std::string>& keys, std::string_view prefix, std::string_view id) {
    const auto hash = sha256_of(id);
    std::array<char, digest_text_bytes> hex{};
    http_encode_lower_hex(hex.data(), hash);
    const std::string_view hex_view(hex.data(), hex.size());
    for (const auto shape : db_cache_shapes) {
        keys.push_back(make_id_key_from_hex(prefix, shape, hex_view));
    }
}
std::string db_cache_key(std::string_view prefix, std::string_view id, db_cache_shape shape, std::string_view sql, std::span<const db_value> params, db_driver driver) {
    if (!id.empty()) {
        return make_id_key(prefix, shape, sha256_of(id));
    }
    std::string result_value;
    result_value.reserve(prefix.size() + 4 + digest_text_bytes);
    result_value.append(prefix);
    sha256 input;
    input.update_u64_le(static_cast<std::uint64_t>(driver));
    digest_text(input, sql);
    input.update_u64_le(params.size());
    for (const auto& value : params) {
        const auto type = value.type();
        const auto type_byte = static_cast<char>(type);
        input.update({&type_byte, 1});
        switch (type) {
            case db_value_type::null:
                break;
            case db_value_type::string:
                digest_text(input, value.text());
                break;
            case db_value_type::signed_value:
                input.update_u64_le(static_cast<std::uint64_t>(value.signed_value()));
                break;
            case db_value_type::unsigned_value:
                input.update_u64_le(value.unsigned_value());
                break;
            case db_value_type::double_value:
                input.update_u64_le(std::bit_cast<std::uint64_t>(value.double_value()));
                break;
            case db_value_type::bool_value:
                input.update_u64_le(value.bool_value());
                break;
        }
    }
    result_value.append("sql:");
    http_append_lower_hex(result_value, input.finish());
    return result_value;
}

namespace {

[[noreturn]] void throw_db_cache_timeout() {
    throw db_error(db_error::code_type::timeout, std::nullopt, "database query cache operation timed out");
}

// The options of one Redis operation under the deadline it shares with the
// query it caches.
[[nodiscard]] operation_options db_cache_options(const operation_options& base, const operation_deadline& timeout) {
    if (timeout.expired()) {
        throw_db_cache_timeout();
    }
    return db_remaining_options(base, timeout);
}

bool is_fatal_redis_cache_error(const redis_error& error) noexcept {
    return error.code() == redis_error::code_type::cancelled || error.code() == redis_error::code_type::closing;
}

void rethrow_or_timeout_redis_cache_error(const redis_error& error, const operation_deadline& deadline, bool ignore_errors) {
    if (is_fatal_redis_cache_error(error)) {
        throw;
    }
    if (deadline.expired()) {
        throw_db_cache_timeout();
    }
    if (!ignore_errors) {
        throw;
    }
}

// Rows of a cached entry, or nullopt for a corrupt entry the cache ignores.
// The bytes are released before the caller goes on to the database.
[[nodiscard]] std::optional<db_rows> decode_cached_rows(std::string bytes_value, const operation_deadline& deadline, bool ignore_errors) {
    std::optional<db_rows> rows;
    try {
        rows.emplace(decode_db_cache_rows(std::move(bytes_value)));
    } catch (const db_conversion_error&) {
        if (deadline.expired()) {
            throw_db_cache_timeout();
        }
        if (!ignore_errors) {
            throw;
        }
        return std::nullopt;
    }
    if (deadline.expired()) {
        throw_db_cache_timeout();
    }
    return rows;
}

task<void> finish_delete_key_batch(std::optional<task<std::int64_t>> deletion, const operation_deadline& deadline_value) {
    if (!deletion) {
        co_return;
    }
    try {
        (void)co_await std::move(*deletion);
    } catch (const redis_error& error) {
        rethrow_or_timeout_redis_cache_error(error, deadline_value, false);
    }
    if (deadline_value.expired()) {
        throw_db_cache_timeout();
    }
}

task<void> delete_key_batch(const redis_session& redis, std::span<const std::string> keys, const operation_options& options, const operation_deadline& deadline_value) {
    if (keys.empty()) {
        return finish_delete_key_batch(std::nullopt, deadline_value);
    }
    try {
        static constexpr std::size_t inline_capacity = 64;
        if (keys.size() <= inline_capacity) {
            std::array<std::string_view, inline_capacity> inline_views;
            for (std::size_t i = 0; i < keys.size(); ++i) {
                inline_views[i] = keys[i];
            }
            // DEL owns its arguments before returning its cold operation.
            auto deletion = redis.with_options(db_cache_options(options, deadline_value)).del(std::span<const std::string_view>(inline_views.data(), keys.size()));
            return finish_delete_key_batch(std::move(deletion), deadline_value);
        }
        std::vector<std::string_view> heap_views;
        heap_views.reserve(keys.size());
        for (const auto& key : keys) {
            heap_views.emplace_back(key);
        }
        auto deletion = redis.with_options(db_cache_options(options, deadline_value)).del(heap_views);
        return finish_delete_key_batch(std::move(deletion), deadline_value);
    } catch (const redis_error& error) {
        rethrow_or_timeout_redis_cache_error(error, deadline_value, false);
        throw;
    }
}

task<void> remove_keys(redis_session redis, std::vector<std::string> keys, request_io_metrics* io_metrics, operation_options options, stop_token shutdown) {
    stop_group cancellation{options.stop_token_, std::move(shutdown)};
    options.stop_token_ = cancellation.token();
    scoped_request_io_timer timer(io_metrics, request_io_kind::redis);
    const operation_deadline deadline_value(options.timeout_);
    auto deletion = delete_key_batch(redis, keys, options, deadline_value);
    // The command owns the keys; release their source before network IO.
    std::vector<std::string>(keys).swap(keys);
    co_await std::move(deletion);
}

task<void> clear_keys(redis_session redis, std::string pattern, request_io_metrics* io_metrics, operation_options options, stop_token shutdown) {
    stop_group cancellation{options.stop_token_, std::move(shutdown)};
    options.stop_token_ = cancellation.token();
    scoped_request_io_timer timer(io_metrics, request_io_kind::redis);
    const operation_deadline deadline_value(options.timeout_);
    std::optional<redis_scan_cursor> cursor;
    do {
        std::optional<redis_scan_result> result;
        try {
            result.emplace(co_await redis.with_options(db_cache_options(options, deadline_value)).scan({.cursor_ = cursor, .match_ = std::string_view(pattern), .count_ = 256}));
        } catch (const redis_error& error) {
            rethrow_or_timeout_redis_cache_error(error, deadline_value, false);
        }
        if (deadline_value.expired()) {
            throw_db_cache_timeout();
        }
        cursor = result->next_cursor();
        auto deletion = delete_key_batch(redis, result->values(), options, deadline_value);
        // Preserve only the cursor while the owned DEL waits for its reply.
        result.reset();
        co_await std::move(deletion);
    } while (cursor);
}

}  // namespace

db_query_cache_state::db_query_cache_state(const redis_session& redis, const db_cache_config_storage& config, std::string prefix) :
    worker_(redis.worker()),
    duration_(config.duration_),
    always_enabled_(config.always_enabled_),
    ignore_errors_(config.ignore_errors_),
    max_entry_reply_bytes_(redis_client_runtime::session_config(redis).max_reply_bytes_),
    prefix_(std::move(prefix)),
    redis_(redis.with_options({})),
    fills_() {}

db_query_cache_state::~db_query_cache_state() = default;

// The leader's hold on the fill it listed. Every exit -- a result, a failure,
// a cancellation or a timeout -- unlists the fill and ends it, so its waiters
// never outlive the leader's attempt.
class db_query_cache_state::fill_lead final {
public:
    fill_lead(db_query_cache_state& state, fill_pointer listed) noexcept :
        state_(state),
        fill_(std::move(listed)) {}
    fill_lead(const fill_lead&) = delete;
    fill_lead& operator=(const fill_lead&) = delete;
    ~fill_lead() {
        finish();
    }

    [[nodiscard]] fill& get() const noexcept {
        return *fill_;
    }
    void finish() noexcept {
        if (!finished_) {
            finished_ = true;
            state_.unlist_fill(*fill_);
            fill_->done_.request_stop();
        }
    }

private:
    db_query_cache_state& state_;
    fill_pointer fill_;
    bool finished_{false};
};

void db_query_cache_state::close_now() noexcept {
    closed_ = true;
    stop_.request_stop();
}

void db_query_cache_state::require_open() const {
    if (closed_) {
        throw db_error(db_error::code_type::closing, std::nullopt, "database query cache is closing");
    }
}

void db_query_cache_state::unlist_fill(const fill& ended) noexcept {
    std::erase_if(fills_, [&ended](const fill_pointer& listed) noexcept {
        return listed.get() == &ended;
    });
}

void db_query_cache_state::unlist_fills(std::span<const std::string> keys) noexcept {
    std::erase_if(fills_, [keys](const fill_pointer& listed) noexcept {
        return std::ranges::find(keys, listed->key_) != keys.end();
    });
}

void db_query_cache_state::unlist_all_fills() noexcept {
    fills_.clear();
}

std::optional<std::string> db_query_cache_state::key(const db_query& query, const db_statement& statement, db_driver driver) {
    if (!query.cache_enabled().value_or(always_enabled_) || !query.cacheable()) {
        return std::nullopt;
    }
    return db_cache_key(prefix_, query.cache_id(), query.cache_shape(), statement.sql(), statement.params(), driver);
}

task<db_rows> db_query_cache_state::wrap(std::optional<std::chrono::milliseconds> duration, std::string key, db_cache_query database, operation_options options, operation_deadline deadline) {
    require_open();
    return query(std::move(key), duration.value_or(duration_), std::move(database), std::move(options), deadline);
}

// Key, deferred database statement and options belong to this cold frame. On
// a hit the statement is destroyed without starting. Decoded rows own their
// fields independently of the Redis reply and later cache operations.
//
// A miss joins the fill already running for the same key on this worker, if
// any (Pingora read_through), and waits for it within the caller's own
// deadline and stop token: the leader's published result answers it like a
// hit, and a leader that ended without one sends it back to the cache, where
// the first to miss again leads the next fill. Each worker fills a key at most
// once at a time; other workers and processes fill it independently.
task<db_rows> db_query_cache_state::query(std::string key, std::chrono::milliseconds duration, db_cache_query database, operation_options options, operation_deadline deadline) {
    stop_group cancellation{options.stop_token_, stop_.token()};
    options.stop_token_ = cancellation.token();
    const auto started = std::chrono::steady_clock::now();
    for (;;) {
        std::optional<std::string> bytes_value;
        try {
            bytes_value = co_await redis_.with_options(db_cache_options(options, deadline)).get(key);
        } catch (const redis_error& error) {
            rethrow_or_timeout_redis_cache_error(error, deadline, ignore_errors_);
        }
        if (bytes_value.has_value()) {
            if (auto rows = decode_cached_rows(std::move(*bytes_value), deadline, ignore_errors_)) {
                co_return std::move(*rows);
            }
        }
        const auto running = std::ranges::find_if(fills_, [&key](const fill_pointer& listed) noexcept {
            return listed->key_ == key;
        });
        if (running == fills_.end()) {
            break;
        }
        const auto joined = *running;
        joined->awaited_ = true;
        timer_sleep_result woken{};
        {
            stop_group wake{options.stop_token_, joined->done_.token()};
            woken = co_await sleep_until(worker_, deadline.deadline().value_or(std::chrono::steady_clock::time_point::max()), wake.token());
        }
        if (options.stop_token_.stop_requested()) {
            require_open();
            throw db_error(db_error::code_type::cancelled, std::nullopt, "database query cache operation cancelled");
        }
        if (joined->result_.has_value()) {
            if (auto rows = decode_cached_rows(std::string(*joined->result_), deadline, ignore_errors_)) {
                co_return std::move(*rows);
            }
        }
        if (deadline.expired()) {
            throw_db_cache_timeout();
        }
        // Cut short with the fill still running: the worker is stopping.
        if (woken == timer_sleep_result::stop_requested && !joined->done_.stop_requested()) {
            throw db_error(db_error::code_type::closing, std::nullopt, "database query cache is closing");
        }
    }

    auto listed = std::make_shared<fill>(key);
    fills_.push_back(listed);
    fill_lead lead(*this, std::move(listed));
    if (deadline.expired()) {
        throw_db_cache_timeout();
    }
    auto rows = co_await std::move(database)(db_remaining_options(options, deadline));
    if (deadline.expired()) {
        throw_db_cache_timeout();
    }
    const auto elapsed = std::chrono::ceil<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
    auto& filled = lead.get();
    const bool store = elapsed < duration;
    if (!store && !filled.awaited_) {
        co_return rows;
    }
    auto encoded_bytes = encode_db_cache_rows(rows);
    // An entry whose GET reply would exceed the store's reply limit would fail
    // every hit and cost a Redis connection each time; like Pingora's
    // ResponseTooLarge, the result stays valid but is not cached.
    const bool fits = !max_entry_reply_bytes_.has_value() || resp_bulk_serialized_size(encoded_bytes.size()) <= *max_entry_reply_bytes_;
    std::optional<task<redis_set_result>> write;
    if (store && fits) {
        try {
            // SET owns its arguments before returning the cold operation.
            write.emplace(redis_.with_options(db_cache_options(options, deadline)).set(key, encoded_bytes, {.expiration_ = redis_set_expiration::expires_after(duration - elapsed)}));
        } catch (const redis_error& error) {
            rethrow_or_timeout_redis_cache_error(error, deadline, ignore_errors_);
        }
    }
    // Waiters take the result from memory; neither the key nor the serialized
    // rows are retained here while the SET waits on IO.
    if (filled.awaited_) {
        filled.result_ = std::move(encoded_bytes);
    }
    std::string(encoded_bytes).swap(encoded_bytes);
    std::string(key).swap(key);
    lead.finish();
    if (write.has_value()) {
        try {
            (void)co_await std::move(*write);
        } catch (const redis_error& error) {
            rethrow_or_timeout_redis_cache_error(error, deadline, ignore_errors_);
        }
        if (deadline.expired()) {
            throw_db_cache_timeout();
        }
    }
    co_return rows;
}

// A cache id names every result shape a repository builder derived from it.
task<void> db_query_cache_state::remove(std::span<const std::string_view> ids, request_io_metrics* io_metrics, operation_options options) {
    require_open();
    for (const auto id : ids) {
        if (id.empty()) {
            throw std::invalid_argument("cache identifier must not be empty");
        }
    }
    std::vector<std::string> keys;
    keys.reserve(ids.size() * db_cache_shapes.size());
    for (const auto id : ids) {
        append_db_cache_id_keys(keys, prefix_, id);
    }
    unlist_fills(keys);
    return remove_keys(redis_, std::move(keys), io_metrics, std::move(options), stop_.token());
}

task<void> db_query_cache_state::clear(request_io_metrics* io_metrics, operation_options options) {
    require_open();
    std::string pattern;
    pattern.reserve(prefix_.size() + 1);
    pattern.append(prefix_);
    pattern.push_back('*');
    unlist_all_fills();
    return clear_keys(redis_, std::move(pattern), io_metrics, std::move(options), stop_.token());
}
}  // namespace ruvia::detail

namespace ruvia {
db_query_result_cache::db_query_result_cache(detail::db_query_cache_state& state_value, operation_scope& scope, detail::request_io_metrics* io_metrics, operation_options options) noexcept :
    state_(&state_value),
    io_metrics_(io_metrics),
    options_(std::move(options)),
    registration_(scope, this, &db_query_result_cache::expire_capability) {}

db_query_result_cache::db_query_result_cache(const db_query_result_cache& other) noexcept :
    state_(other.state_),
    io_metrics_(other.io_metrics_),
    options_(other.options_),
    registration_(other.registration_, this) {}

void db_query_result_cache::expire_capability(void* target) noexcept {
    auto& cache = *static_cast<db_query_result_cache*>(target);
    cache.state_ = nullptr;
    cache.io_metrics_ = nullptr;
    cache.options_ = {};
}
task<void> db_query_result_cache::remove(std::span<const std::string_view> ids) const {
    registration_.require_active();
    return make_scoped_operation(registration_.scope(), state_->remove(ids, io_metrics_, options_));
}
task<void> db_query_result_cache::clear() const {
    registration_.require_active();
    return make_scoped_operation(registration_.scope(), state_->clear(io_metrics_, options_));
}
namespace detail {
namespace {
// PostgreSQL's lock_timeout is an int32 millisecond setting. Keep the
// backend range check before connecting rather than narrowing or silently
// shortening a configured migration lock wait.
[[nodiscard]] std::uint64_t postgres_lock_timeout_milliseconds(timeout_duration timeout) {
    if (timeout.value().count() <= 0) {
        throw std::invalid_argument("database migration lock timeout must be greater than zero");
    }
    if (timeout.value().count() > std::numeric_limits<std::int32_t>::max()) {
        throw std::invalid_argument("database migration lock timeout cannot be represented as PostgreSQL milliseconds");
    }
    return static_cast<std::uint64_t>(timeout.value().count());
}

// A migration table name is a SQL identifier that cannot be parameterized, so
// it is restricted to [A-Za-z0-9_] before being quoted -- a configuration
// policy on top of the dialect's quoting rule, which owns the length limit.
[[nodiscard]] bool is_valid_migration_table_name(std::string_view name) noexcept {
    if (name.empty()) {
        return false;
    }
    for (const auto ch : name) {
        const auto c = static_cast<unsigned char>(ch);
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_') {
            continue;
        }
        return false;
    }
    return true;
}

// MariaDB's collations are PAD SPACE, including the binary one the migrations
// table pins, so "v1" and "v1 " are one id there and two everywhere else. An id
// wrapped in whitespace is a typo in every case that matters, so it is refused
// rather than quietly folded.
[[nodiscard]] bool has_surrounding_whitespace(std::string_view id) noexcept {
    return !id.empty() && (is_sql_whitespace(id.front()) || is_sql_whitespace(id.back()));
}

// Two migration ids are the same id to the schema table if they differ only in
// ASCII letter case. The applied-migration lookup compares them with the
// column's collation, and MariaDB's default (utf8mb4_general_ci) is
// case-insensitive: "v1" and "V1" collide there while PostgreSQL keeps them
// apart, so one list would produce two different schemas. New tables pin a
// binary collation, but a table created before that still compares loosely, so
// the ambiguity is refused at the source instead.
[[nodiscard]] bool migration_ids_collide(std::string_view left, std::string_view right) noexcept {
    return http_ascii_equals_ignore_case(left, right);
}

// A migration is one statement. Both backends enforce that anyway -- libpq's
// extended protocol rejects multiple commands outright and the MariaDB
// connection never enables CLIENT_MULTI_STATEMENTS -- but they report it as a
// backend syntax error pointing at the second statement, which reads like the
// SQL is wrong rather than the packaging. A trailing separator is accepted by
// both, so only a separator with statement text after it is refused.
[[nodiscard]] std::size_t skip_migration_trailing_comment(std::string_view sql, std::size_t index, db_driver driver) noexcept {
    if (index >= sql.size()) {
        return index;
    }
    if (driver == db_driver::mariadb && sql[index] == '#') {
        return skip_sql_line_comment(sql, index + 1, false);
    }
    if (driver == db_driver::mariadb ? is_mariadb_double_dash_comment(sql, index) : (sql[index] == '-' && index + 1 < sql.size() && sql[index + 1] == '-')) {
        return skip_sql_line_comment(sql, index + 2, driver == db_driver::postgresql);
    }
    if (sql[index] == '/' && index + 1 < sql.size() && sql[index + 1] == '*') {
        return driver == db_driver::postgresql ? skip_postgresql_block_comment(sql, index) : skip_sql_block_comment(sql, index);
    }
    return index;
}

[[nodiscard]] bool has_trailing_sql_only(std::string_view sql, std::size_t after, db_driver driver) noexcept {
    for (auto index = after; index < sql.size();) {
        if (!is_sql_whitespace(sql[index])) {
            const auto next_value = skip_migration_trailing_comment(sql, index, driver);
            if (next_value == index) {
                return false;
            }
            index = next_value;
            continue;
        }
        ++index;
    }
    return true;
}

[[nodiscard]] bool has_migration_statement_text(std::string_view sql, db_driver driver) noexcept {
    for (auto index = std::size_t{0}; index < sql.size();) {
        if (is_sql_whitespace(sql[index]) || sql[index] == ';') {
            ++index;
            continue;
        }
        const auto next_value = skip_migration_trailing_comment(sql, index, driver);
        if (next_value != index) {
            index = next_value;
            continue;
        }
        return true;
    }
    return false;
}

[[nodiscard]] std::size_t find_migration_statement_separator(std::string_view sql, db_driver driver) {
    switch (driver) {
        case db_driver::mariadb:
            return find_sql_syntax_byte(sql, ';', true);
        case db_driver::postgresql:
            return find_postgresql_syntax_byte(sql, ';');
        default:
            throw std::invalid_argument("database driver is invalid");
    }
}

// Validates a developer-supplied migration list before it is applied: every id
// must be non-empty, at most 190 bytes (the indexed schema column width), have
// non-empty single-statement SQL, and be unique -- a duplicate id would run the
// wrong migration.
void validate_migration_list(std::span<const db_migration> migrations, db_driver driver) {
    for (std::size_t i = 0; i < migrations.size(); ++i) {
        const auto& migration = migrations[i];
        switch (migration.atomicity()) {
            case db_migration_atomicity::transactional:
            case db_migration_atomicity::unwrapped:
                break;
            default:
                throw std::invalid_argument("database migration atomicity is invalid");
        }
        if (migration.id().empty()) {
            throw std::invalid_argument("database migration id must not be empty");
        }
        if (migration.id().find('\0') != std::string_view::npos) {
            throw std::invalid_argument("database migration id must not contain NUL bytes");
        }
        if (migration.id().size() > 190) {
            throw std::invalid_argument("database migration id must not exceed 190 bytes");
        }
        if (has_surrounding_whitespace(migration.id())) {
            throw std::invalid_argument("database migration id must not begin or end with whitespace");
        }
        if (!has_migration_statement_text(migration.sql(), driver)) {
            throw std::invalid_argument("database migration SQL must not be empty");
        }
        if (migration.sql().find('\0') != std::string_view::npos) {
            throw std::invalid_argument("database migration SQL must not contain NUL bytes");
        }
        const auto separator = find_migration_statement_separator(migration.sql(), driver);
        if (separator != std::string_view::npos && !has_trailing_sql_only(migration.sql(), separator + 1, driver)) {
            throw std::invalid_argument("database migration must contain exactly one SQL statement");
        }
        for (std::size_t j = i + 1; j < migrations.size(); ++j) {
            if (migration_ids_collide(migrations[j].id(), migration.id())) {
                throw std::invalid_argument("database migration ids must be unique, including case");
            }
        }
    }
}

}  // namespace
}  // namespace detail

namespace {

struct db_migrator_options_storage final {
    db_migrator_options_storage(const db_migrator_options& source_value) :
        table_(source_value.table_),
        lock_timeout_(source_value.lock_timeout_) {}

    db_migrator_options_storage(const db_migrator_options_storage& source_value) :
        table_(source_value.table_),
        lock_timeout_(source_value.lock_timeout_) {}

    std::string table_;
    timeout_duration lock_timeout_;
};

void validate_db_migrator_options(const db_migrator_options& options, db_driver driver) {
    if (!detail::is_valid_migration_table_name(options.table_)) {
        throw std::invalid_argument("database migration table has an invalid backend identifier");
    }
    detail::validate_db_identifier(options.table_, driver);
    if (options.lock_timeout_.value().count() <= 0) {
        throw std::invalid_argument("database migration lock timeout must be greater than zero");
    }
    if (driver == db_driver::postgresql) {
        // Check the backend's millisecond range before opening a connection.
        (void)detail::postgres_lock_timeout_milliseconds(options.lock_timeout_);
    }
}

[[nodiscard]] std::string build_migration_lock_name(const detail::db_config_storage& config) {
    constexpr std::string_view prefix = "ruvia:migrations:";
    std::string name;
    name.reserve(prefix.size() + (!config.database_.empty() ? config.database_.size() : config.host_.size() + 1 + 10));
    name.append(prefix);
    if (!config.database_.empty()) {
        name.append(config.database_);
    } else {
        name.append(config.host_);
        name.push_back(':');
        detail::append_db_number(name, static_cast<std::uint64_t>(config.port_));
    }
    return name;
}

[[nodiscard]] std::string build_create_migrations_table_sql(std::string_view table_value, db_driver driver) {
    std::string sql;
    sql.reserve(table_value.size() + 260);
    sql.append("CREATE TABLE IF NOT EXISTS ");
    detail::append_db_identifier(sql, table_value, driver);
    if (driver == db_driver::postgresql) {
        // timestamptz records the instant; PostgreSQL's plain timestamp would
        // record a wall-clock reading whose zone nobody wrote down.
        sql.append(
            " (migration_id VARCHAR(190) PRIMARY KEY,"
            " applied_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,");
    } else {
        // A binary collation keeps ids that differ only in letter case
        // distinct, matching PostgreSQL. It is still PAD SPACE, so trailing
        // spaces remain invisible to a comparison here; ids carrying them are
        // refused before they reach this table. DATETIME rather than TIMESTAMP:
        // TIMESTAMP is a 32-bit epoch that stops in 2038 and is rewritten
        // across session time zones.
        sql.append(
            " (migration_id VARCHAR(190) CHARACTER SET utf8mb4 COLLATE utf8mb4_bin PRIMARY KEY,"
            " applied_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,");
    }
    // The CHECK is deliberately unnamed: MySQL scopes CHECK constraint names to
    // the schema, so a fixed name would collide the moment a second migration
    // table is created in one database.
    sql.append(" checksum CHAR(64) NOT NULL, CHECK (migration_id <> ''))");
    return sql;
}

// Verify existing history even for an empty migration list. A missing digest
// column cannot be reconstructed from today's SQL without inventing history.
[[nodiscard]] constexpr std::string_view checksum_column_probe_sql(db_driver driver) noexcept {
    return driver == db_driver::postgresql ? "SELECT 1 FROM information_schema.columns WHERE table_schema = current_schema() AND table_name = $1 AND column_name = 'checksum'" : "SELECT 1 FROM information_schema.columns WHERE table_schema = DATABASE() AND table_name = ? AND column_name = 'checksum'";
}

[[nodiscard]] std::string build_find_migration_sql(std::string_view table_value, db_driver driver) {
    std::string sql;
    sql.reserve(table_value.size() + 50);
    sql.append("SELECT checksum FROM ");
    detail::append_db_identifier(sql, table_value, driver);
    sql.append(driver == db_driver::postgresql ? " WHERE migration_id = $1 LIMIT 1" : " WHERE migration_id = ? LIMIT 1");
    return sql;
}

[[nodiscard]] std::string build_insert_migration_sql(std::string_view table_value, db_driver driver) {
    std::string sql;
    sql.reserve(table_value.size() + 40);
    sql.append("INSERT INTO ");
    detail::append_db_identifier(sql, table_value, driver);
    sql.append(driver == db_driver::postgresql ? " (migration_id, checksum) VALUES ($1, $2)" : " (migration_id, checksum) VALUES (?, ?)");
    return sql;
}

void append_migration_id(std::vector<std::string>& ids, std::string_view id) {
    ids.emplace_back();
    ids.back().assign(id.data(), id.size());
}

[[nodiscard]] std::runtime_error migration_drift(std::string_view id) {
    constexpr std::string_view prefix = "database migration '";
    constexpr std::string_view suffix = "' cannot be verified; its recorded checksum is missing or does not match";
    std::string message;
    message.reserve(prefix.size() + id.size() + suffix.size());
    message.append(prefix);
    message.append(id);
    message.append(suffix);
    return std::runtime_error(message.c_str());
}

// A migration is identified by its id for the rest of a schema's life, so
// editing one that has already run changes nothing: the id is on record and the
// new text is skipped, silently, on every machine that already migrated. The
// digest of the text is recorded alongside the id so that edit is reported
// instead.
//
// SHA-256, lowercase hex. This is drift detection, not a security boundary --
// nobody is choosing migration text to collide with -- but a digest with no
// known collisions costs nothing here and leaves no judgement call.
[[nodiscard]] std::string migration_checksum(std::string_view sql) {
    std::string hex;
    http_append_lower_hex(hex, detail::sha256_of(sql));
    return hex;
}

}  // namespace

class detail::db_migration_runner final {
public:
    // Every input is owned by this frame, so the run outlives neither the
    // migrator nor the caller's migration list.
    [[nodiscard]] static task<db_migration_report> run(event_loop loop, db_config_storage config, std::vector<db_migration> migrations, db_migrator_options_storage options, operation_options operation) {
        if (!loop.worker().is_current()) {
            throw std::logic_error("database migration must run on its bound event loop");
        }
        const auto driver = config.driver_;

        // The lock is held by the session, so every migration statement and
        // history row must run on the session that took it: one connection,
        // which never reconnects. A statement that ends or poisons the session
        // (a lost connection, a migration leaving a transaction open) fails
        // every statement after it instead of running them unserialized.
        // transaction_timeout_ stays as configured, like query_timeout_: a
        // transactional migration that outlives it fails with timeout and its
        // session closes, ending the run with the lock released.
        config.pool_size_per_worker_ = 1;
        config.reconnect_ = false;
        if (!config.acquire_timeout_.has_value()) {
            config.acquire_timeout_ = config.query_timeout_;
        }
        db_migration_report report;

        auto lock_name = build_migration_lock_name(config);
        const detail::db_definition databases[] = {detail::db_definition{std::string(detail::default_capability_alias), std::move(config)}};
        detail::db_registry registry(loop.io_context(), loop.worker(), databases);
        // One bound for the whole run, connection included: its expiry or stop
        // request closes the dedicated connection, which also releases the lock.
        operation_interrupt_source interrupts;
        std::optional<operation_interrupt> interrupt;
        try {
            interrupt.emplace(interrupts, loop.worker(), operation, &registry, &close_registry, "database migration");
        } catch (const std::system_error&) {
            throw db_error(db_error::code_type::cancelled, driver, "database migration cancelled");
        }

        std::exception_ptr failure;
        try {
            co_await registry.connect();
            ::ruvia::operation_scope operation_scope;
            auto handle = registry.get(operation_scope, nullptr);
            co_await acquire_lock(handle, driver, lock_name, options.lock_timeout_);
            try {
                co_await apply_migrations(handle, driver, migrations, options, report);
            } catch (...) {
                failure = std::current_exception();
            }
            // Release explicitly only after a clean run. After a failure the
            // session may already be gone, or still be inside a transaction a
            // failed statement left; closing it below releases the lock in
            // every case, since both lock flavours end with their session.
            if (failure == nullptr) {
                co_await release_lock(handle, driver, lock_name);
            }
        } catch (...) {
            failure = std::current_exception();
        }

        registry.close_now();
        if (const auto cause = interrupt->cause()) {
            throw db_error(*cause == operation_interrupt_cause::timed_out ? db_error::code_type::timeout : db_error::code_type::cancelled, driver, *cause == operation_interrupt_cause::timed_out ? "database migration timed out" : "database migration cancelled");
        }
        if (failure != nullptr) {
            std::rethrow_exception(failure);
        }
        co_return std::move(report);
    }

private:
    static void close_registry(void* registry) noexcept {
        static_cast<detail::db_registry*>(registry)->close_now();
    }

private:
    // Serializes concurrent deployers. Both locks are held by the session, so
    // losing the connection releases them -- including the migration's own
    // failure path, which closes it.
    [[nodiscard]] static task<void> acquire_lock(db_session& handle, db_driver driver, std::string_view lock_name, timeout_duration lock_timeout) {
        if (driver == db_driver::mariadb) {
            // Milliseconds are already bounded by int64; rounding after
            // division by 1000 keeps the SQL integer safely in range.
            const auto lock_seconds = std::chrono::ceil<std::chrono::seconds>(lock_timeout.value()).count();
            std::array<db_value, 2> lock_params{db_value{lock_name}, db_value{lock_seconds}};
            auto lock_result = co_await handle.query("SELECT GET_LOCK(?, ?)", std::span<const db_value>(lock_params));
            // GET_LOCK answers 0 on timeout and NULL on error rather than
            // failing the statement, so the wait has to be read out of the row.
            if (lock_result.size() != 1 || lock_result[0].empty() || lock_result[0][0].as<bool>() != true) {
                throw std::runtime_error("database migration lock could not be acquired");
            }
            co_return;
        }

        // PostgreSQL's advisory lock waits without a bound of its own; the
        // session's lock_timeout is what ends that wait. It bounds only this
        // wait: migration statements run on the same session and must keep the
        // server's configured lock_timeout.
        auto set_timeout = [&] {
            std::string timeout_sql;
            timeout_sql.reserve(48);
            timeout_sql.append("SET lock_timeout TO '");
            detail::append_db_number(timeout_sql, detail::postgres_lock_timeout_milliseconds(lock_timeout));
            timeout_sql.append("ms'");
            return handle.execute(timeout_sql);
        }();
        (void)co_await std::move(set_timeout);
        std::array<db_value, 1> lock_params{db_value{lock_name}};
        (void)co_await handle.query("SELECT pg_advisory_lock(hashtextextended($1, 0))", std::span<const db_value>(lock_params));
        (void)co_await handle.execute("RESET lock_timeout");
    }

    [[nodiscard]] static task<void> release_lock(db_session& handle, db_driver driver, std::string_view lock_name) {
        std::array<db_value, 1> release_params{db_value{lock_name}};
        if (driver == db_driver::mariadb) {
            (void)co_await handle.execute("DO RELEASE_LOCK(?)", std::span<const db_value>(release_params));
        } else {
            (void)co_await handle.query("SELECT pg_advisory_unlock(hashtextextended($1, 0))", std::span<const db_value>(release_params));
        }
    }

    [[nodiscard]] static task<void> apply_migrations(db_session& handle, db_driver driver, std::span<const db_migration> migrations, const db_migrator_options_storage& options, db_migration_report& report) {
        auto create_table = [&] {
            const auto sql = build_create_migrations_table_sql(options.table_, driver);
            return handle.execute(sql);
        }();
        (void)co_await std::move(create_table);

        std::array<db_value, 1> table_params{db_value{std::string_view(options.table_)}};
        if ((co_await handle.query(checksum_column_probe_sql(driver), std::span<const db_value>(table_params))).empty()) {
            throw std::runtime_error("database migration history has no checksum column");
        }
        if (migrations.empty()) {
            co_return;
        }

        auto find_sql = build_find_migration_sql(options.table_, driver);
        auto insert_sql = build_insert_migration_sql(options.table_, driver);
        for (const auto& migration : migrations) {
            const auto checksum = migration_checksum(migration.sql());
            std::array<db_value, 1> find_params{db_value{migration.id()}};
            auto existing = co_await handle.query(find_sql, std::span<const db_value>(find_params));
            if (!existing.empty()) {
                const auto value = existing[0][0].value();
                if (!value || *value != std::string_view(checksum)) {
                    throw migration_drift(migration.id());
                }
                append_migration_id(report.skipped_, migration.id());
                continue;
            }

            std::array<db_value, 2> insert_params{db_value{migration.id()}, db_value{std::string_view(checksum)}};
            // On PostgreSQL the statement and the row recording it commit
            // together, so an interruption between them cannot leave the schema
            // changed and unrecorded. MariaDB commits DDL implicitly, so there
            // is no transaction to put them in and the two-statement window
            // stands.
            if (driver == db_driver::postgresql && migration.atomicity() == db_migration_atomicity::transactional) {
                auto transaction = co_await handle.begin_transaction();
                (void)co_await transaction.execute(migration.sql());
                (void)co_await transaction.execute(insert_sql, std::span<const db_value>(insert_params));
                co_await transaction.commit();
            } else {
                (void)co_await handle.execute(migration.sql());
                (void)co_await handle.execute(insert_sql, std::span<const db_value>(insert_params));
            }
            append_migration_id(report.applied_, migration.id());
        }
    }
};

class db_migrator::storage_type final {
public:
    storage_type(event_loop loop, detail::validated_db_config_view config_source_value, const db_migrator_options& options_source) :
        loop_(std::move(loop)),
        config_(config_source_value),
        options_(options_source) {}

    event_loop loop_;
    detail::db_config_storage config_;
    db_migrator_options_storage options_;
};

void db_migrator::storage_deleter_type::operator()(storage_type* storage) const noexcept {
    delete storage;
}

db_migrator::storage_owner_type db_migrator::make_storage(event_loop loop, const db_config& config, const db_migrator_options& options) {
    if (!loop.valid()) {
        throw std::invalid_argument("database migrator requires a valid event loop");
    }
    const auto validated_config = detail::validated_db_config(config);
    validate_db_migrator_options(options, validated_config.get().driver_);
    detail::require_client_host_lookup(validated_config.get().host_, loop.worker());
    return storage_owner_type(new storage_type(std::move(loop), validated_config, options), storage_deleter_type{});
}

db_migrator::db_migrator(event_loop loop, const db_config& config, const db_migrator_options& options) :
    storage_(make_storage(std::move(loop), config, options)) {}

db_migrator::~db_migrator() = default;

db_migrator::db_migrator(db_migrator&&) noexcept = default;

db_migrator& db_migrator::operator=(db_migrator&&) noexcept = default;

task<db_migration_report> db_migrator::migrate(std::span<const db_migration> migrations, operation_options operation) const& {
    if (storage_ == nullptr) {
        throw std::logic_error("database migrator has been moved from");
    }
    detail::validate_migration_list(migrations, storage_->config_.driver_);
    return detail::db_migration_runner::run(storage_->loop_, detail::db_config_storage(storage_->config_), std::vector<db_migration>(migrations.begin(), migrations.end()), db_migrator_options_storage(storage_->options_), std::move(operation));
}

}  // namespace ruvia

#endif
