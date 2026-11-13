#include <algorithm>
#include <array>
#include <exception>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <variant>

#include "ruvia/core/event_loop.h"
#include "ruvia/http/http_header.h"

#include "route_table.h"

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
#include <ranges>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

#include "ruvia/core/event_loop.h"
#include "ruvia/http/http_header.h"
#include "ruvia/web/db/db.h"
#include "ruvia/web/redis/redis.h"

#include "route_table.h"
namespace ruvia::detail {
namespace {

void append_list_separator(std::string& output) {
    if (!output.empty()) {
        output.push_back(',');
    }
}

void append_db_resolved_address(db_resolved_addresses_type& addresses,
    const asio::ip::address& endpoint, db_driver driver) {
    std::string address;
    try {
        address = endpoint.to_string();
    } catch (const std::system_error& error) {
        throw db_error(db_error::code_type::resolve_failed, driver,
            system_error_text(error), error.code().value());
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

[[nodiscard]] db_transaction_start_plan make_db_transaction_start_plan(
    db_driver driver, db_transaction_options options) {
    const auto isolation = static_cast<unsigned>(options.isolation_);
    const auto access = static_cast<unsigned>(options.access_mode_);
    if (isolation > static_cast<unsigned>(db_transaction_isolation::serializable) ||
        access > static_cast<unsigned>(db_transaction_access_mode::read_only)) {
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
        static constexpr std::array<std::string_view, 5> configure{
            "", "SET TRANSACTION ISOLATION LEVEL READ UNCOMMITTED",
            "SET TRANSACTION ISOLATION LEVEL READ COMMITTED",
            "SET TRANSACTION ISOLATION LEVEL REPEATABLE READ",
            "SET TRANSACTION ISOLATION LEVEL SERIALIZABLE"};
        static constexpr std::array<std::string_view, 3> commands{
            "START TRANSACTION", "START TRANSACTION READ WRITE", "START TRANSACTION READ ONLY"};
        return {.configure_ = configure[isolation], .begin_ = commands[access]};
    }
    throw std::invalid_argument("transaction options require a database driver");
}

}  // namespace

db_resolved_addresses_type collect_db_resolved_addresses(const client_resolve_result& results,
    db_driver driver) {
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
        throw db_error(
            db_error::code_type::resolve_failed, driver, "database host resolved to no addresses");
    }
    return addresses;
}

std::string make_mariadb_resolved_host_list(
    std::span<const std::string> addresses) {
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

postgresql_resolved_host_list make_postgresql_resolved_host_list(std::string_view logical_host,
    std::span<const std::string> addresses) {
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
            throw db_error(db_error::code_type::timeout, pool.config_.driver_,
                "database connection pool acquire timed out");
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
db_pool<driver_type>::db_pool(asio::io_context& io_context, const worker_ref& worker_value,
    db_config_storage config)
    : io_context_(io_context),
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
    return db_error(db_error::code_type::timeout, driver_type::driver,
        std::string(driver_type::name).append(" operation timed out"));
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
operation_deadline db_pool<driver_type>::lease_operation_timeout(
    std::size_t index, const operation_deadline& timeout) const noexcept {
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
    return db_error(db_error::code_type::timeout, driver_type::driver,
        std::string(driver_type::name).append(" transaction or stream exceeded transaction_timeout"));
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
        throw db_error(db_error::code_type::io_error, driver_type::driver,
            std::string(driver_type::name).append(" session ended, and this pool does not reconnect"));
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
task<db_resolved_addresses_type> db_pool<driver_type>::resolve_host(
    std::size_t index, const operation_deadline& deadline_value) {
    const auto resolve_timed_out = [] {
        return db_error(db_error::code_type::timeout, driver_type::driver,
            std::string(driver_type::name).append(" host resolve timed out"));
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
            throw db_error(db_error::code_type::resolve_failed, driver_type::driver,
                std::string("resolving ").append(driver_type::name).append(" host failed: ").append(error_code_text(resolve_error)),
                resolve_error.value());
        }
        co_return collect_db_resolved_addresses(completion, driver_type::driver);
    } catch (...) {
        (void)leases_.clear_deadline(index);
        throw;
    }
}

template <typename driver_type>
db_pool<driver_type>::slot_wait::slot_wait(db_pool& pool, std::size_t index, db_deadline_kind kind,
    db_slot_socket::native_socket_type native, db_socket_readiness wanted,
    std::optional<std::chrono::steady_clock::time_point> wait_deadline, const operation_deadline& operation_deadline_value) noexcept
    : pool_(pool),
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
task<result_type> db_pool<driver_type>::run_on_transaction_slot(std::size_t index, std::string sql,
    std::vector<db_value> params, const operation_options& options,
    statement_hook_type<result_type> statement) {
    require_slot_index(index);
    const auto timeout = lease_operation_timeout(index, operation_deadline(options.timeout_));
    typename leases_type::cancellation_guard cancellation(leases_, index, options.stop_token_);
    std::exception_ptr failure;
    try {
        enter_lease_operation(index);
        if (!slot(index).connected_) {
            co_await connect_slot(index, timeout);
        }
        auto result_value = co_await statement(*this, index, sql, std::span<const db_value>(params),
            timeout.constrained_by(config_.query_timeout_));
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
    return run_pipelined<typename driver_type::pipeline_io_type, db_rows>(
        *this, std::move(sql), std::move(params), std::move(options), io_metrics);
}

template <typename driver_type>
task<db_exec_result> db_pool<driver_type>::execute(std::string sql, std::vector<db_value> params, operation_options options, request_io_metrics* io_metrics) {
    return run_pipelined<typename driver_type::pipeline_io_type, db_exec_result>(
        *this, std::move(sql), std::move(params), std::move(options), io_metrics);
}

template <typename driver_type>
task<db_rows> db_pool<driver_type>::query_on_transaction_slot(std::size_t slot, std::string sql,
    std::vector<db_value> params, const operation_options& options) {
    return run_on_transaction_slot<db_rows>(
        slot, std::move(sql), std::move(params), options, &driver_type::query);
}

template <typename driver_type>
task<db_exec_result> db_pool<driver_type>::execute_on_transaction_slot(std::size_t slot, std::string sql,
    std::vector<db_value> params, const operation_options& options) {
    return run_on_transaction_slot<db_exec_result>(
        slot, std::move(sql), std::move(params), options, &driver_type::execute);
}

template <typename driver_type>
task<void> db_pool<driver_type>::execute_control(std::size_t index, std::string_view sql, const operation_deadline& timeout) {
    leases_.throw_if_aborted(index);
    if (!slot(index).connected_) {
        co_await connect_slot(index, timeout);
    }
    (void)co_await driver_type::execute(
        *this, index, sql, {}, timeout.constrained_by(config_.query_timeout_));
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
task<void> db_pool<driver_type>::commit_transaction(
    std::size_t slot, const operation_options& options) {
    return finish_transaction(slot, "COMMIT", options);
}

template <typename driver_type>
task<void> db_pool<driver_type>::rollback_transaction(
    std::size_t slot, const operation_options& options) {
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
void db_pool<driver_type>::fail_stream(
    std::size_t index, typename leases_type::cancellation_guard& cancellation) noexcept {
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
        co_return db_stream_result(
            db_pool_ref_type{this}, index, std::move(start.column_names_), std::move(options));
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
task<std::optional<db_row>> db_pool<driver_type>::read_stream_row(std::size_t index,
    std::vector<db_field>& fields, std::vector<std::string>& column_names, const operation_options& options) {
    require_slot_index(index);
    const auto timeout = lease_operation_timeout(index, operation_deadline(options.timeout_));
    typename leases_type::cancellation_guard cancellation(leases_, index, options.stop_token_);
    std::optional<db_stream_step> step;
    try {
        enter_lease_operation(index);
        step.emplace(co_await driver_type::read_stream_row(*this, index, slot(index).stream_result_, fields,
            column_names, timeout.constrained_by(config_.query_timeout_)));
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
        co_await driver_type::close_stream(
            *this, index, slot(index).stream_result_, timeout.constrained_by(config_.query_timeout_));
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

}  // namespace ruvia::detail

namespace ruvia::detail {

#if defined(_WIN32)
db_slot_socket::db_slot_socket(asio::io_context& io_context)
    : socket_(io_context) {}
#else
db_slot_socket::db_slot_socket(asio::io_context& io_context)
    : descriptor_(io_context) {}
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
    socket_.waitable().async_wait(
        wait_type, [this, readiness](std::error_code wait_error) noexcept { complete(readiness, wait_error); });
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
    if (protocol_info.iSocketType != SOCK_STREAM ||
        (protocol_info.iAddressFamily != AF_INET && protocol_info.iAddressFamily != AF_INET6)) {
        return std::make_error_code(std::errc::address_family_not_supported);
    }
    const SOCKET duplicate = ::WSASocketW(FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO,
        &protocol_info, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
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
constexpr std::size_t digest_text_bytes = 2 * db_sha256::digest_size;

[[nodiscard]] std::string make_id_key_from_hex(std::string_view prefix, db_cache_shape shape,
    std::string_view digest_hex) {
    const auto shape_tag = db_cache_shape_tag(shape);
    std::string result;
    result.reserve(prefix.size() + 3 + shape_tag.size() + 1 + digest_hex.size());
    result.append(prefix).append("id:").append(shape_tag).push_back(':');
    result.append(digest_hex);
    return result;
}

[[nodiscard]] std::string make_id_key(std::string_view prefix, db_cache_shape shape,
    const db_sha256::digest_type& hash) {
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
void digest_text(db_sha256& digest, std::string_view value) {
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
        rows.push_back(db_result_access::borrowed_row(
            fields_value.data() + row_start, static_cast<std::size_t>(columns), names.data(), names.size()));
    }
    if (!reader_value.bytes_.empty()) {
        invalid_cache();
    }
    return result_value;
}
std::string db_cache_prefix(std::string_view name_space, std::string_view alias,
    const db_config_storage& config) {
    const auto server_name = config.tls_.server_name_ ? std::string_view(*config.tls_.server_name_) : std::string_view{};
    db_sha256 identity;
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
void append_db_cache_id_keys(
    std::vector<std::string>& keys, std::string_view prefix, std::string_view id) {
    const auto hash = db_sha256_of(id);
    std::array<char, digest_text_bytes> hex{};
    http_encode_lower_hex(hex.data(), hash);
    const std::string_view hex_view(hex.data(), hex.size());
    for (const auto shape : db_cache_shapes) {
        keys.push_back(make_id_key_from_hex(prefix, shape, hex_view));
    }
}
std::string db_cache_key(std::string_view prefix, std::string_view id, db_cache_shape shape,
    std::string_view sql, std::span<const db_value> params, db_driver driver) {
    if (!id.empty()) {
        return make_id_key(prefix, shape, db_sha256_of(id));
    }
    std::string result_value;
    result_value.reserve(prefix.size() + 4 + digest_text_bytes);
    result_value.append(prefix);
    db_sha256 input;
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
    return error.code() == redis_error::code_type::cancelled ||
           error.code() == redis_error::code_type::closing;
}

void rethrow_or_timeout_redis_cache_error(
    const redis_error& error, const operation_deadline& deadline, bool ignore_errors) {
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
            auto deletion = redis.with_options(db_cache_options(options, deadline_value))
                                .del(std::span<const std::string_view>(inline_views.data(), keys.size()));
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

task<void> remove_keys(redis_session redis, std::vector<std::string> keys, request_io_metrics* io_metrics,
    operation_options options, stop_token shutdown) {
    stop_group cancellation{options.stop_token_, std::move(shutdown)};
    options.stop_token_ = cancellation.token();
    scoped_request_io_timer timer(io_metrics, request_io_kind::redis);
    const operation_deadline deadline_value(options.timeout_);
    auto deletion = delete_key_batch(redis, keys, options, deadline_value);
    // The command owns the keys; release their source before network IO.
    std::vector<std::string>(keys).swap(keys);
    co_await std::move(deletion);
}

task<void> clear_keys(
    redis_session redis, std::string pattern, request_io_metrics* io_metrics, operation_options options,
    stop_token shutdown) {
    stop_group cancellation{options.stop_token_, std::move(shutdown)};
    options.stop_token_ = cancellation.token();
    scoped_request_io_timer timer(io_metrics, request_io_kind::redis);
    const operation_deadline deadline_value(options.timeout_);
    std::optional<redis_scan_cursor> cursor;
    do {
        std::optional<redis_scan_result> result;
        try {
            result.emplace(co_await redis.with_options(db_cache_options(options, deadline_value))
                    .scan({.cursor_ = cursor, .match_ = std::string_view(pattern), .count_ = 256}));
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

db_query_cache_state::db_query_cache_state(const redis_session& redis, const db_cache_config_storage& config,
    std::string prefix)
    : worker_(redis.worker()),
      duration_(config.duration_),
      always_enabled_(config.always_enabled_),
      ignore_errors_(config.ignore_errors_),
      max_entry_reply_bytes_(redis_client_runtime::handle_config(redis).max_reply_bytes_),
      prefix_(std::move(prefix)),
      redis_(redis.with_options({})),
      fills_() {}

db_query_cache_state::~db_query_cache_state() = default;

// The leader's hold on the fill it listed. Every exit -- a result, a failure,
// a cancellation or a timeout -- unlists the fill and ends it, so its waiters
// never outlive the leader's attempt.
class db_query_cache_state::fill_lead final {
public:
    fill_lead(db_query_cache_state& state, fill_pointer listed) noexcept
        : state_(state),
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

std::optional<std::string> db_query_cache_state::key(
    const db_query& query, const db_statement& statement, db_driver driver) {
    if (!query.cache_enabled().value_or(always_enabled_) || !query.cacheable()) {
        return std::nullopt;
    }
    return db_cache_key(prefix_, query.cache_id(), query.cache_shape(), statement.sql(), statement.params(), driver);
}

task<db_rows> db_query_cache_state::wrap(std::optional<std::chrono::milliseconds> duration, std::string key,
    db_cache_query database, operation_options options, operation_deadline deadline) {
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
task<db_rows> db_query_cache_state::query(std::string key, std::chrono::milliseconds duration,
    db_cache_query database, operation_options options, operation_deadline deadline) {
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
            woken = co_await sleep_until(
                worker_, deadline.deadline().value_or(std::chrono::steady_clock::time_point::max()), wake.token());
        }
        if (options.stop_token_.stop_requested()) {
            require_open();
            throw db_error(db_error::code_type::cancelled, std::nullopt, "database query cache operation cancelled");
        }
        if (joined->result_.has_value()) {
            if (auto rows = decode_cached_rows(std::string(*joined->result_), deadline,
                    ignore_errors_)) {
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
    const auto elapsed =
        std::chrono::ceil<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
    auto& filled = lead.get();
    const bool store = elapsed < duration;
    if (!store && !filled.awaited_) {
        co_return rows;
    }
    auto encoded_bytes = encode_db_cache_rows(rows);
    // An entry whose GET reply would exceed the store's reply limit would fail
    // every hit and cost a Redis connection each time; like Pingora's
    // ResponseTooLarge, the result stays valid but is not cached.
    const bool fits = !max_entry_reply_bytes_.has_value() ||
                      resp_bulk_serialized_size(encoded_bytes.size()) <= *max_entry_reply_bytes_;
    std::optional<task<redis_set_result>> write;
    if (store && fits) {
        try {
            // SET owns its arguments before returning the cold operation.
            write.emplace(redis_.with_options(db_cache_options(options, deadline))
                    .set(key, encoded_bytes, {.expiration_ = redis_set_expiration::expires_after(duration - elapsed)}));
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
task<void> db_query_cache_state::remove(
    std::span<const std::string_view> ids, request_io_metrics* io_metrics, operation_options options) {
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
db_query_result_cache::db_query_result_cache(detail::db_query_cache_state& state_value, operation_scope& scope,
    detail::request_io_metrics* io_metrics, operation_options options) noexcept
    : state_(&state_value),
      io_metrics_(io_metrics),
      options_(std::move(options)),
      registration_(scope, this, &db_query_result_cache::expire_capability) {}

db_query_result_cache::db_query_result_cache(const db_query_result_cache& other) noexcept
    : state_(other.state_),
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
}  // namespace ruvia

namespace ruvia {
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
        throw std::invalid_argument(
            "database migration lock timeout cannot be represented as PostgreSQL milliseconds");
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
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '_') {
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
[[nodiscard]] bool migration_ids_collide(
    std::string_view left, std::string_view right) noexcept {
    return http_ascii_equals_ignore_case(left, right);
}

// A migration is one statement. Both backends enforce that anyway -- libpq's
// extended protocol rejects multiple commands outright and the MariaDB
// connection never enables CLIENT_MULTI_STATEMENTS -- but they report it as a
// backend syntax error pointing at the second statement, which reads like the
// SQL is wrong rather than the packaging. A trailing separator is accepted by
// both, so only a separator with statement text after it is refused.
[[nodiscard]] std::size_t skip_migration_trailing_comment(
    std::string_view sql, std::size_t index, db_driver driver) noexcept {
    if (index >= sql.size()) {
        return index;
    }
    if (driver == db_driver::mariadb && sql[index] == '#') {
        return skip_sql_line_comment(sql, index + 1, false);
    }
    if (driver == db_driver::mariadb
            ? is_mariadb_double_dash_comment(sql, index)
            : (sql[index] == '-' && index + 1 < sql.size() && sql[index + 1] == '-')) {
        return skip_sql_line_comment(sql, index + 2, driver == db_driver::postgresql);
    }
    if (sql[index] == '/' && index + 1 < sql.size() && sql[index + 1] == '*') {
        return driver == db_driver::postgresql ? skip_postgresql_block_comment(sql, index)
                                               : skip_sql_block_comment(sql, index);
    }
    return index;
}

[[nodiscard]] bool has_trailing_sql_only(
    std::string_view sql, std::size_t after, db_driver driver) noexcept {
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

[[nodiscard]] bool has_migration_statement_text(
    std::string_view sql, db_driver driver) noexcept {
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

[[nodiscard]] std::size_t find_migration_statement_separator(
    std::string_view sql, db_driver driver) {
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
            throw std::invalid_argument(
                "database migration id must not begin or end with whitespace");
        }
        if (!has_migration_statement_text(migration.sql(), driver)) {
            throw std::invalid_argument("database migration SQL must not be empty");
        }
        if (migration.sql().find('\0') != std::string_view::npos) {
            throw std::invalid_argument("database migration SQL must not contain NUL bytes");
        }
        const auto separator = find_migration_statement_separator(migration.sql(), driver);
        if (separator != std::string_view::npos &&
            !has_trailing_sql_only(migration.sql(), separator + 1, driver)) {
            throw std::invalid_argument(
                "database migration must contain exactly one SQL statement");
        }
        for (std::size_t j = i + 1; j < migrations.size(); ++j) {
            if (migration_ids_collide(migrations[j].id(), migration.id())) {
                throw std::invalid_argument(
                    "database migration ids must be unique, including case");
            }
        }
    }
}

}  // namespace
}  // namespace detail

namespace {

struct db_migrator_options_storage final {
    db_migrator_options_storage(const db_migrator_options& source_value)
        : table_(source_value.table_),
          lock_timeout_(source_value.lock_timeout_) {}

    db_migrator_options_storage(
        const db_migrator_options_storage& source_value)
        : table_(source_value.table_),
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

[[nodiscard]] std::string build_migration_lock_name(
    const detail::db_config_storage& config) {
    constexpr std::string_view prefix = "ruvia:migrations:";
    std::string name;
    name.reserve(prefix.size() +
                 (!config.database_.empty() ? config.database_.size() : config.host_.size() + 1 + 10));
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

[[nodiscard]] std::string build_create_migrations_table_sql(
    std::string_view table_value, db_driver driver) {
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
    return driver == db_driver::postgresql
               ? "SELECT 1 FROM information_schema.columns WHERE table_schema = current_schema() AND table_name = $1 AND column_name = 'checksum'"
               : "SELECT 1 FROM information_schema.columns WHERE table_schema = DATABASE() AND table_name = ? AND column_name = 'checksum'";
}

[[nodiscard]] std::string build_find_migration_sql(
    std::string_view table_value, db_driver driver) {
    std::string sql;
    sql.reserve(table_value.size() + 50);
    sql.append("SELECT checksum FROM ");
    detail::append_db_identifier(sql, table_value, driver);
    sql.append(driver == db_driver::postgresql ? " WHERE migration_id = $1 LIMIT 1"
                                               : " WHERE migration_id = ? LIMIT 1");
    return sql;
}

[[nodiscard]] std::string build_insert_migration_sql(
    std::string_view table_value, db_driver driver) {
    std::string sql;
    sql.reserve(table_value.size() + 40);
    sql.append("INSERT INTO ");
    detail::append_db_identifier(sql, table_value, driver);
    sql.append(driver == db_driver::postgresql ? " (migration_id, checksum) VALUES ($1, $2)"
                                               : " (migration_id, checksum) VALUES (?, ?)");
    return sql;
}

void append_migration_id(std::vector<std::string>& ids, std::string_view id) {
    ids.emplace_back();
    ids.back().assign(id.data(), id.size());
}

[[nodiscard]] std::runtime_error migration_drift(
    std::string_view id) {
    constexpr std::string_view prefix = "database migration '";
    constexpr std::string_view suffix =
        "' cannot be verified; its recorded checksum is missing or does not match";
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
    http_append_lower_hex(hex, detail::db_sha256_of(sql));
    return hex;
}

}  // namespace

class detail::db_migration_runner final {
public:
    // Every input is owned by this frame, so the run outlives neither the
    // migrator nor the caller's migration list.
    [[nodiscard]] static task<db_migration_report> run(event_loop loop, db_config_storage config,
        std::vector<db_migration> migrations, db_migrator_options_storage options, operation_options operation) {
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
        const detail::db_definition databases[] = {
            detail::db_definition{std::string(detail::default_capability_alias),
                std::move(config)}};
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
            throw db_error(*cause == operation_interrupt_cause::timed_out ? db_error::code_type::timeout
                                                                          : db_error::code_type::cancelled,
                driver, *cause == operation_interrupt_cause::timed_out ? "database migration timed out" : "database migration cancelled");
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
    [[nodiscard]] static task<void> acquire_lock(db_session& handle, db_driver driver,
        std::string_view lock_name, timeout_duration lock_timeout) {
        if (driver == db_driver::mariadb) {
            // Milliseconds are already bounded by int64; rounding after
            // division by 1000 keeps the SQL integer safely in range.
            const auto lock_seconds = std::chrono::ceil<std::chrono::seconds>(lock_timeout.value()).count();
            std::array<db_value, 2> lock_params{db_value{lock_name}, db_value{lock_seconds}};
            auto lock_result = co_await handle.query(
                "SELECT GET_LOCK(?, ?)", std::span<const db_value>(lock_params));
            // GET_LOCK answers 0 on timeout and NULL on error rather than
            // failing the statement, so the wait has to be read out of the row.
            if (lock_result.size() != 1 || lock_result[0].empty() ||
                lock_result[0][0].as<bool>() != true) {
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
        (void)co_await handle.query("SELECT pg_advisory_lock(hashtextextended($1, 0))",
            std::span<const db_value>(lock_params));
        (void)co_await handle.execute("RESET lock_timeout");
    }

    [[nodiscard]] static task<void> release_lock(
        db_session& handle, db_driver driver, std::string_view lock_name) {
        std::array<db_value, 1> release_params{db_value{lock_name}};
        if (driver == db_driver::mariadb) {
            (void)co_await handle.execute(
                "DO RELEASE_LOCK(?)", std::span<const db_value>(release_params));
        } else {
            (void)co_await handle.query("SELECT pg_advisory_unlock(hashtextextended($1, 0))",
                std::span<const db_value>(release_params));
        }
    }

    [[nodiscard]] static task<void> apply_migrations(db_session& handle, db_driver driver,
        std::span<const db_migration> migrations, const db_migrator_options_storage& options,
        db_migration_report& report) {
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

            std::array<db_value, 2> insert_params{
                db_value{migration.id()}, db_value{std::string_view(checksum)}};
            // On PostgreSQL the statement and the row recording it commit
            // together, so an interruption between them cannot leave the schema
            // changed and unrecorded. MariaDB commits DDL implicitly, so there
            // is no transaction to put them in and the two-statement window
            // stands.
            if (driver == db_driver::postgresql &&
                migration.atomicity() == db_migration_atomicity::transactional) {
                auto transaction = co_await handle.begin_transaction();
                (void)co_await transaction.execute(migration.sql());
                (void)co_await transaction.execute(
                    insert_sql, std::span<const db_value>(insert_params));
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
    storage_type(event_loop loop, detail::validated_db_config_view config_source_value,
        const db_migrator_options& options_source)
        : loop_(std::move(loop)),
          config_(config_source_value),
          options_(options_source) {}

    event_loop loop_;
    detail::db_config_storage config_;
    db_migrator_options_storage options_;
};

void db_migrator::storage_deleter_type::operator()(storage_type* storage) const noexcept {
    delete storage;
}

db_migrator::storage_owner_type db_migrator::make_storage(
    event_loop loop, const db_config& config, const db_migrator_options& options) {
    if (!loop.valid()) {
        throw std::invalid_argument("database migrator requires a valid event loop");
    }
    const auto validated_config = detail::validated_db_config(config);
    validate_db_migrator_options(options, validated_config.get().driver_);
    detail::require_client_host_lookup(validated_config.get().host_, loop.worker());
    return storage_owner_type(
        new storage_type(std::move(loop), validated_config, options), storage_deleter_type{});
}

db_migrator::db_migrator(event_loop loop, const db_config& config, const db_migrator_options& options)
    : storage_(make_storage(std::move(loop), config, options)) {}

db_migrator::~db_migrator() = default;

db_migrator::db_migrator(db_migrator&&) noexcept = default;

db_migrator& db_migrator::operator=(db_migrator&&) noexcept = default;

task<db_migration_report> db_migrator::migrate(
    std::span<const db_migration> migrations, operation_options operation) const& {
    if (storage_ == nullptr) {
        throw std::logic_error("database migrator has been moved from");
    }
    detail::validate_migration_list(migrations, storage_->config_.driver_);
    return detail::db_migration_runner::run(storage_->loop_, detail::db_config_storage(storage_->config_),
        std::vector<db_migration>(migrations.begin(), migrations.end()),
        db_migrator_options_storage(storage_->options_), std::move(operation));
}

}  // namespace ruvia

#endif

#ifdef RUVIA_ENABLE_REDIS
#endif

// The per-request database handle: it borrows a registry entry for the
// operation's lifetime and hands each call to the pool.

namespace ruvia {

namespace {

task<void> discard_rows_task(task<db_rows> rows_task) {
    (void)co_await std::move(rows_task);
}

}  // namespace

db_session::db_session(detail::db_pool_ref_type client,
    ::ruvia::operation_scope& operation_scope, detail::request_io_metrics* io_metrics,
    detail::db_query_cache_state* cache) noexcept
    : cache_(cache),
      client_(client),
      io_metrics_(io_metrics),
      registration_(operation_scope, this, &db_session::expire_capability) {}

db_session::db_session(const db_session& other) noexcept
    : cache_(other.cache_),
      client_(other.client_),
      io_metrics_(other.io_metrics_),
      options_(other.options_),
      registration_(other.registration_, this) {}

#ifdef RUVIA_ENABLE_REDIS
db_query_result_cache db_session::query_result_cache() const {
    registration_.require_active();
    if (!cache_) {
        throw db_error(db_error::code_type::not_configured, std::nullopt, "database query cache is not configured");
    }
    return db_query_result_cache(*cache_, registration_.scope(), io_metrics_, options_);
}
#endif

db_session db_session::with_options(operation_options options) const {
    registration_.require_active();
    db_session copy(*this);
    copy.options_ = override_operation_options(options_, std::move(options));
    return copy;
}

const worker_ref& db_session::worker() const& {
    registration_.require_active();
    return detail::visit_db_pool(client_, [](const auto& client) -> const worker_ref& {
        return client.worker();
    });
}

void db_session::expire_capability(void* target) noexcept {
    auto& handle = *static_cast<db_session*>(target);
    handle.cache_ = nullptr;
    handle.client_ = detail::db_pool_ref_type{};
    handle.io_metrics_ = nullptr;
    handle.options_ = {};
}

db_driver db_session::query_driver() const {
    registration_.require_active();
    return detail::db_pool_driver(client_);
}

detail::db_query_backend db_session::query_backend() const {
    detail::db_query_backend backend{client_, std::nullopt, io_metrics_};
#ifdef RUVIA_ENABLE_REDIS
    backend.cache_ = cache_;
#endif
    return backend;
}

task<db_rows> db_session::query_task(const db_query& query) const {
    registration_.require_active();
    const auto backend = query_backend();
    auto plan = detail::db_query_plan::prepare(query, nullptr, backend);
#ifdef RUVIA_ENABLE_REDIS
    if (backend.cache_ != nullptr && plan.first_.cache_key_.has_value()) {
        return detail::execute_db_query_plan<false>(std::move(plan), backend, options_);
    }
#endif
    auto options_copy = options_;
    return detail::query_db_backend(
        backend, std::move(plan.first_.sql_), std::move(plan.first_.params_), options_copy);
}

task<std::pair<db_rows, db_rows>> db_session::query_and_count_task(const db_query& query, const db_query& count) const {
    registration_.require_active();
    const auto backend = query_backend();
    auto plan = detail::db_query_plan::prepare(query, &count, backend);
    return detail::execute_db_query_plan<true>(std::move(plan), backend, options_);
}

task<db_exec_result> db_session::execute_query(const db_query& query) const {
    registration_.require_active();
    auto statement = query.compile(query_driver());
    if (statement.returns_rows()) {
        throw std::invalid_argument("execute requires a statement without returned rows");
    }
    return ::ruvia::make_scoped_operation(registration_.scope(),
        detail::visit_db_pool(client_, [&](auto& client) {
            return client.execute(
                std::move(statement.sql_), std::move(statement.params_), options_, io_metrics_);
        }));
}

task<db_exec_result> db_session::execute_prepared(std::string sql) const {
    registration_.require_active();
    return ::ruvia::make_scoped_operation(registration_.scope(),
        detail::visit_db_pool(client_, [&](auto& client) {
            return client.execute(
                std::move(sql), std::vector<db_value>{}, options_, io_metrics_);
        }));
}

task<db_rows> db_session::query(
    std::string_view sql, std::span<const db_value> params) const {
    registration_.require_active();
    auto statement = prepare_db_statement(sql, params);
    return ::ruvia::make_scoped_operation(registration_.scope(),
        detail::visit_db_pool(client_, [&](auto& client) {
            return client.query(
                std::move(statement.sql_), std::move(statement.params_), options_, io_metrics_);
        }));
}

task<db_exec_result> db_session::execute(
    std::string_view sql, std::span<const db_value> params) const {
    registration_.require_active();
    auto statement = prepare_db_statement(sql, params);
    return ::ruvia::make_scoped_operation(registration_.scope(),
        detail::visit_db_pool(client_, [&](auto& client) {
            return client.execute(
                std::move(statement.sql_), std::move(statement.params_), options_, io_metrics_);
        }));
}

task<db_stream_result> db_session::query_stream(
    std::string_view sql, std::span<const db_value> params) const {
    registration_.require_active();
    auto statement = prepare_db_statement(sql, params);
    auto& scope = registration_.scope();
    return ::ruvia::make_scoped_operation(scope, query_stream_prepared(client_, std::move(statement.sql_),
                                                     std::move(statement.params_), scope, io_metrics_, options_));
}

task<db_stream_result> db_session::query_stream_prepared(detail::db_pool_ref_type client, std::string sql,
    std::vector<db_value> params,
    ::ruvia::operation_scope& operation_scope, detail::request_io_metrics* io_metrics, operation_options options) {
    detail::scoped_request_io_timer timer(io_metrics, detail::request_io_kind::db);
    auto result_value = co_await detail::visit_db_pool(client, [&](auto& pool) {
        return pool.stream(std::move(sql), std::move(params), std::move(options));
    });
    result_value.bind_operation_scope(operation_scope, io_metrics);
    co_return result_value;
}

task<db_transaction> db_session::begin_transaction(db_transaction_options options) const {
    registration_.require_active();
    auto& scope = registration_.scope();
    return ::ruvia::make_scoped_operation(scope,
        begin_transaction_prepared(query_backend(), scope, options_, std::move(options)));
}

task<db_transaction> db_session::begin_transaction_prepared(detail::db_query_backend backend,
    ::ruvia::operation_scope& operation_scope, operation_options operation_options_value,
    db_transaction_options transaction_options) {
    detail::scoped_request_io_timer timer(backend.io_metrics_, detail::request_io_kind::db);
    auto transaction = co_await detail::visit_db_pool(backend.pool_, [&](auto& pool) {
        return pool.begin_transaction(std::move(operation_options_value), std::move(transaction_options));
    });
    transaction.bind_operation_scope(operation_scope, backend.io_metrics_);
    co_return transaction;
}

task<void> db_session::notify(std::string_view channel, std::string_view payload) const {
    registration_.require_active();
    if (query_driver() != db_driver::postgresql) {
        throw std::invalid_argument("database notify requires PostgreSQL");
    }
    auto statement = prepare_db_notify(channel, payload);
    auto& scope = registration_.scope();
    return ::ruvia::make_scoped_operation(scope,
        discard_rows_task(detail::visit_db_pool(client_, [&](auto& client) {
            return client.query(std::move(statement.sql_), std::move(statement.params_), options_, io_metrics_);
        })));
}

db_session db_session::bind_caller_scope(::ruvia::operation_scope& scope,
    detail::request_io_metrics* io_metrics, const stop_token& stop_token_value) && {
    registration_.require_active();
    db_session bound(client_, scope, io_metrics, cache_);
    bound.options_ = override_operation_options(options_, operation_options{.timeout_ = std::nullopt, .stop_token_ = stop_token_value});
    return bound;
}

}  // namespace ruvia

namespace ruvia::detail {

class db_client_state final : public std::enable_shared_from_this<db_client_state> {
public:
    db_client_state(event_loop loop, const db_config& config);
#ifdef RUVIA_ENABLE_REDIS
    db_client_state(event_loop loop, const db_config& config, const redis_session& cache_store,
        const db_cache_config& cache_policy);
#endif

    db_client_state(const db_client_state&) = delete;
    db_client_state& operator=(const db_client_state&) = delete;

    void bind_stop() {
        lifecycle_.bind_stop();
    }
    [[nodiscard]] task<void> connect() {
        return lifecycle_.connect();
    }
    void request_close() noexcept {
        lifecycle_.request_close();
    }
    [[nodiscard]] task<void> shutdown() {
        return lifecycle_.shutdown();
    }
    [[nodiscard]] db_session db();

    [[nodiscard]] const worker_ref& worker() const noexcept {
        return worker_;
    }

private:
    friend class client_lifecycle<db_client_state>;
    static constexpr std::string_view client_name{"database"};
    [[noreturn]] static void throw_not_ready();
    [[nodiscard]] db_registry& backend() noexcept {
        return databases_;
    }

    event_loop loop_;
    worker_ref worker_;
    db_registry databases_;
    // Retires scopes before backend storage and its allocator are destroyed.
    client_lifecycle<db_client_state> lifecycle_;
};
db_client_state::db_client_state(event_loop loop, const db_config& config)
    : loop_(client_lifecycle<db_client_state>::require_loop(std::move(loop))),
      worker_(loop_.worker()),
      databases_(loop_.io_context(), worker_, config),
      lifecycle_(*this, loop_, worker_) {}

#ifdef RUVIA_ENABLE_REDIS
db_client_state::db_client_state(event_loop loop, const db_config& config,
    const redis_session& cache_store, const db_cache_config& cache_policy)
    : loop_(client_lifecycle<db_client_state>::require_loop(std::move(loop))),
      worker_(loop_.worker()),
      databases_(loop_.io_context(), worker_, config, cache_store, cache_policy),
      lifecycle_(*this, loop_, worker_) {}
#endif

void db_client_state::throw_not_ready() {
    throw std::logic_error("database client is not connected");
}

db_session db_client_state::db() {
    lifecycle_.require_ready();
    return databases_.get(lifecycle_.operation_scope(), nullptr);
}

}  // namespace ruvia::detail

namespace ruvia {

db_client::db_client(event_loop loop, const db_config& config)
    : state_(std::make_shared<detail::db_client_state>(std::move(loop), config)) {
    state_->bind_stop();
}

#ifdef RUVIA_ENABLE_REDIS
db_client::db_client(event_loop loop, const db_config& config,
    const redis_session& cache_store, const db_cache_config& cache_policy)
    : state_(std::make_shared<detail::db_client_state>(std::move(loop), config, cache_store, cache_policy)) {
    state_->bind_stop();
}
#endif

db_client::~db_client() {
    state_->request_close();
}

task<void> db_client::connect() & {
    return state_->connect();
}

db_session db_client::db() const& {
    return state_->db();
}

void db_client::close() noexcept {
    state_->request_close();
}

task<void> db_client::shutdown() & {
    return state_->shutdown();
}

const worker_ref& db_client::worker() const& noexcept {
    return state_->worker();
}

}  // namespace ruvia

#ifdef RUVIA_ENABLE_REDIS
#endif

namespace ruvia {
namespace {

[[nodiscard]] detail::db_pool_ref_type pool_ref(const detail::db_registry::pool_owner_type& owner_value) noexcept {
    return std::visit(
        [](const auto& value) {
            return value == nullptr ? detail::db_pool_ref_type{} : detail::db_pool_ref_type{value.get()};
        },
        owner_value);
}

#ifdef RUVIA_ENABLE_REDIS
[[nodiscard]] const detail::db_config_storage& pool_config(
    const detail::db_registry::pool_owner_type& owner_value) noexcept {
    return std::visit(
        [](const auto& value) noexcept -> const detail::db_config_storage& { return value->config(); }, owner_value);
}
#endif

}  // namespace

detail::db_registry::db_registry(asio::io_context& io_context, const worker_ref& worker_value,
    const db_config& default_config)
    : entries_(),
      alias_index_() {
    alias_index_.build({default_capability_alias});
    entries_.reserve(1);
    add(io_context, worker_value, db_config_storage(default_config));
}

detail::db_registry::db_registry(asio::io_context& io_context, const worker_ref& worker_value,
    std::span<const detail::db_definition> databases, redis_registry* redis)
    : entries_(),
      alias_index_() {
    detail::validate_capability_aliases(
        databases, "database alias must not be empty", "duplicate database alias");
    alias_index_.build(databases);
    entries_.reserve(databases.size());
    for (const auto& definition : databases) {
        add(io_context, worker_value, db_config_storage(definition.config_));
#ifdef RUVIA_ENABLE_REDIS
        if (definition.query_cache_) {
            if (redis == nullptr) {
                throw std::invalid_argument("database query cache requires an existing Redis registry");
            }
            const auto store_value = redis->get(definition.query_cache_->redis_alias_, cache_scope_, nullptr);
            attach_cache(entries_.size() - 1, worker_value, store_value, definition.query_cache_->policy_,
                definition.config_, definition.alias_);
        }
#else
        (void)redis;
#endif
    }
}

#ifdef RUVIA_ENABLE_REDIS
detail::db_registry::db_registry(asio::io_context& io_context, const worker_ref& worker_value,
    const db_config& config,
    const redis_session& redis, const db_cache_config& policy)
    : db_registry(io_context, worker_value, config) {
    attach_cache(0, worker_value, redis, db_cache_config_storage(policy),
        pool_config(entries_[0].pool_), default_capability_alias);
}

void detail::db_registry::attach_cache(std::size_t index, const worker_ref& worker_value,
    const redis_session& redis, const db_cache_config_storage& policy,
    const db_config_storage& config, std::string_view alias) {
    if (redis.worker().id() != worker_value.id()) {
        throw std::invalid_argument("database query cache Redis capability must belong to the same worker");
    }
    auto prefix = db_cache_prefix(policy.key_prefix_, alias, config);
    entries_[index].cache_ = std::make_unique<db_query_cache_state>(redis, policy, std::move(prefix));
}
#endif

detail::db_registry::~db_registry() = default;

detail::db_registry::entry_type::entry_type(pool_owner_type pool) noexcept
    : pool_(std::move(pool)) {}

void detail::db_registry::add(
    asio::io_context& io_context, const worker_ref& worker_value, db_config_storage config) {
    require_client_host_lookup(config.host_, worker_value);
    pool_owner_type owner;
    switch (config.driver_) {
        case db_driver::unspecified:
            std::terminate();
        case db_driver::mariadb:
#ifdef RUVIA_ENABLE_MARIADB
            owner = std::make_unique<db_pool<mariadb_driver>>(io_context, worker_value, std::move(config));
            break;
#else
            std::terminate();
#endif
        case db_driver::postgresql:
#ifdef RUVIA_ENABLE_POSTGRESQL
            owner = std::make_unique<db_pool<postgresql_driver>>(io_context, worker_value, std::move(config));
            break;
#else
            std::terminate();
#endif
    }

    entries_.emplace_back(std::move(owner));
}

task<void> detail::db_registry::connect() {
    for (auto& entry : entries_) {
        co_await visit_db_pool(pool_ref(entry.pool_), [](auto& pool) { return pool.connect(); });
    }
}

void detail::db_registry::close_now() noexcept {
    for (auto& entry : entries_) {
#ifdef RUVIA_ENABLE_REDIS
        if (entry.cache_) {
            entry.cache_->close_now();
        }
#endif
        visit_db_pool_if_present(pool_ref(entry.pool_), [](auto& pool) noexcept { pool.close_now(); });
    }
}

db_session detail::db_registry::session(
    const entry_type& entry, ::ruvia::operation_scope& operation_scope, request_io_metrics* io_metrics) const {
#ifdef RUVIA_ENABLE_REDIS
    detail::db_query_cache_state* const cache = entry.cache_.get();
#else
    detail::db_query_cache_state* const cache = nullptr;
#endif
    return db_session(pool_ref(entry.pool_), operation_scope, io_metrics, cache);
}

bool detail::db_registry::empty() const noexcept {
    return entries_.empty();
}

db_session detail::db_registry::get(::ruvia::operation_scope& operation_scope, request_io_metrics* io_metrics) const {
    const auto default_pool_index = alias_index_.default_index();
    if (!default_pool_index.has_value()) {
        throw db_error(
            db_error::code_type::not_configured, std::nullopt, "default database is not configured");
    }
    return session(entries_[*default_pool_index], operation_scope, io_metrics);
}

db_session detail::db_registry::get(
    std::string_view alias, ::ruvia::operation_scope& operation_scope, request_io_metrics* io_metrics) const {
    const auto match = alias_index_.find(alias);
    if (match.has_value()) {
        return session(entries_[*match], operation_scope, io_metrics);
    }
    throw db_error(db_error::code_type::not_configured, std::nullopt, "database is not configured");
}

}  // namespace ruvia

namespace ruvia::detail {

const db_column_info& require_db_column(std::span<const db_column_info> columns, std::string_view name) {
    const auto found = std::ranges::find(columns, name, &db_column_info::name_);
    if (found == columns.end()) {
        throw std::invalid_argument("unknown database entity column");
    }
    return *found;
}

void validate_insert_columns(std::span<const db_column_info> entity_columns, std::span<const std::string_view> columns) {
    if (columns.empty()) {
        throw std::invalid_argument("insert_from requires target columns");
    }
    for (const auto column : columns) {
        if (!require_db_column(entity_columns, column).writable_) {
            throw std::invalid_argument("insert_from cannot write a generated column");
        }
    }
}

void apply_entity_upsert(db_query& query, std::span<const db_column_info> entity_columns,
    std::span<db_assignment> updates, std::span<const std::string_view> inserted_columns,
    const db_upsert_options& options, db_driver driver, std::string_view table, std::string_view qualifier,
    std::string_view update_date, std::string_view version) {
    if ((options.skip_update_if_no_values_changed_ || !options.index_predicate_.empty() ||
            !options.update_where_.empty()) &&
        driver != db_driver::postgresql) {
        throw std::invalid_argument("conditional repository upsert requires PostgreSQL");
    }
    for (const auto& name : options.conflict_paths_) {
        require_db_column(entity_columns, name);
    }
    for (const auto& name : options.update_columns_) {
        if (!require_db_column(entity_columns, name).writable_) {
            throw std::invalid_argument("repository upsert cannot update a generated column");
        }
    }
    for (std::size_t i = 0; i < options.update_expressions_.size(); ++i) {
        const auto& item = options.update_expressions_[i];
        if (!require_db_column(entity_columns, item.column_).writable_ || item.empty() || options.do_nothing_) {
            throw std::invalid_argument("upsert expression requires a writable column and update action");
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (options.update_expressions_[j].column_ == item.column_) {
                throw std::invalid_argument("duplicate upsert expression column");
            }
        }
    }
    if (options.do_nothing_ && !options.update_where_.empty()) {
        throw std::invalid_argument("upsert update condition requires an update action");
    }
    const auto alias = qualifier == table ? std::string_view{} : qualifier;
    const auto target_where = options.index_predicate_.expression(query, table, alias);
    std::size_t update_count = 0;
    db_expression update_where;
    bool do_nothing = options.do_nothing_;
    if (!do_nothing) {
        for (const auto& column : entity_columns) {
            const auto name = column.name_;
            const auto custom = std::ranges::find_if(options.update_expressions_,
                [name](const auto& item) { return item.column_ == name; });
            bool selected = custom != options.update_expressions_.end();
            if (!selected) {
                selected = options.update_columns_.empty()
                               ? column.conflict_updatable_ && std::ranges::find(inserted_columns, name) != inserted_columns.end() &&
                                     std::ranges::find(options.conflict_paths_, borrowed_text(name)) == options.conflict_paths_.end()
                               : column.writable_ && std::ranges::find(options.update_columns_, borrowed_text(name)) != options.update_columns_.end();
            }
            if (!selected) {
                continue;
            }
            const auto value = custom == options.update_expressions_.end()
                                   ? query.excluded(name)
                                   : custom->expression(query, table, alias);
            updates[update_count++] = {name, db_expr(value, {}, true)};
            if (options.skip_update_if_no_values_changed_) {
                const auto changed = query.binary(query.column(name, qualifier), db_binary_operator::is_distinct_from, value);
                update_where = update_where.empty() ? changed : query.binary(update_where, db_binary_operator::or_value, changed);
            }
        }
        if (update_count != 0) {
            const auto assigned_count = update_count;
            const auto assigned = [&](std::string_view name) {
                return std::ranges::any_of(updates.first(assigned_count), [name](const auto& item) { return item.column_ == name; });
            };
            append_lifecycle_assignments(query, qualifier, update_date, version, assigned,
                [&](std::string_view name, db_expression value) { updates[update_count++] = {name, db_expr(value, {}, true)}; });
        } else if (driver == db_driver::postgresql) {
            do_nothing = true;
        }
    }
    if (!options.update_where_.empty()) {
        const auto condition = options.update_where_.expression(query, table, alias);
        update_where = update_where.empty() ? condition : query.binary(update_where, db_binary_operator::and_value, condition);
    }
    query.on_conflict({
        .columns_ = options.conflict_paths_,
        .target_where_ = target_where,
        .update_ = std::span<const db_assignment>(updates.data(), update_count),
        .update_where_ = update_where,
        .do_nothing_ = do_nothing,
        .any_unique_key_ = options.any_unique_key_,
    });
}

std::size_t db_relation_plan::find(std::size_t parent_value, std::size_t relation) const noexcept {
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        if (nodes_[i].parent_ == parent_value && nodes_[i].relation_ == relation) {
            return i;
        }
    }
    return root;
}

std::string db_relation_plan::normalize(std::string_view root_alias, std::string_view path) const {
    if (path.size() > root_alias.size() && path.starts_with(root_alias) && path[root_alias.size()] == '.') {
        path.remove_prefix(root_alias.size() + 1);
    } else if (const auto dot = path.find('.'); dot != std::string_view::npos) {
        for (const auto& node : nodes_) {
            if (path.substr(0, dot) == node.alias_) {
                const auto suffix = path.substr(dot);
                std::string result_value;
                result_value.reserve(node.path_.size() + suffix.size());
                result_value.append(node.path_);
                result_value.append(suffix);
                return result_value;
            }
        }
    }
    return std::string(path);
}

bool db_relation_plan::uses_source_name(const db_query& query, std::string_view name) {
    const auto& s = query.storage();
    return (s.source_ && source_binds_name(*s.source_, name)) ||
           std::ranges::any_of(s.joins_, [&](const auto& join) { return source_binds_name(join.source_, name); });
}

bool db_relation_plan::uses_projection_name(const db_query& query, std::string_view name) {
    const auto& s = query.storage();
    return std::ranges::any_of(s.projections_, [&](auto index) {
        const auto& node_value = s.nodes_[index];
        return (node_value.kind_ == db_node_kind::column || node_value.kind_ == db_node_kind::alias) && node_value.text_ == name;
    });
}

std::optional<db_query> db_relation_plan::prepare_entity_read(const db_query& query,
    std::span<const std::string_view> primary_key, std::string_view root_alias, db_driver driver) {
    const auto& s = query.storage();
    if (s.kind_ != db_query_kind::select || primary_key.empty()) {
        throw std::invalid_argument("relation loading requires an entity SELECT with a primary key");
    }
    const bool paginated = s.limit_.has_value() || s.offset_.has_value();
    const bool scope_lock = driver == db_driver::postgresql && s.lock_ && s.lock_->tables_.empty();
    if (!paginated && !scope_lock) {
        return std::nullopt;
    }
    if (paginated) {
        if (query.has_grouping() || !s.set_operations_.empty() || !s.distinct_on_.empty()) {
            throw std::invalid_argument("paged relation loading requires an ungrouped SELECT without set operations or DISTINCT ON");
        }
        if (s.lock_ && s.lock_->skip_locked_) {
            throw std::invalid_argument("paged relation loading cannot combine SKIP LOCKED with collection expansion");
        }
    }
    auto result_value = query.clone();
    if (scope_lock) {
        result_value.storage().lock_->tables_.emplace_back(root_alias);
    }
    if (!paginated) {
        return result_value;
    }

    // Select a page of root identities before filtering the expanded result.
    // Ranking makes joined ordering deterministic for each root, and avoids
    // applying LIMIT to child rows or truncating a loaded collection.
    auto ranked = query.clone();
    auto& rank_storage = ranked.storage();
    // Refer to the outer WITH bindings. Copying their declarations into the
    // page would evaluate volatile CTEs twice and relocate DML CTEs illegally.
    rank_storage.ctes_.clear();
    auto orders = std::move(rank_storage.orders_);
    rank_storage.limit_.reset();
    rank_storage.offset_.reset();
    rank_storage.lock_.reset();
    rank_storage.distinct_ = false;
    for (auto& order : orders) {
        const auto& node_value = rank_storage.nodes_[order.expression_];
        if (node_value.kind_ == db_node_kind::column && node_value.qualifier_.empty()) {
            for (const auto index : rank_storage.projections_) {
                const auto& projection = rank_storage.nodes_[index];
                if (projection.kind_ == db_node_kind::alias && projection.text_ == node_value.text_) {
                    order.expression_ = projection.left_;
                    break;
                }
            }
        }
    }
    rank_storage.projections_.clear();
    auto window = ranked.make_node(db_node_kind::window);
    window.left_ = ranked.require_expression(ranked.call("row_number"));
    window.args_.reserve(primary_key.size());
    std::vector<std::string> key_names;
    key_names.reserve(primary_key.size());
    for (std::size_t i = 0; i < primary_key.size(); ++i) {
        auto& name = key_names.emplace_back();
        name.reserve(32);
        name.append("__ruvia_page_key_");
        append_db_number(name, static_cast<std::uint64_t>(i));
        const auto key = ranked.column(primary_key[i], root_alias);
        window.args_.push_back(ranked.require_expression(key));
        ranked.add_select(ranked.alias(key, name));
    }
    if (orders.empty()) {
        orders.reserve(window.args_.size());
        for (const auto key : window.args_) {
            orders.push_back({key, db_order_direction::asc, db_nulls_order::default_value});
        }
    }
    // Give the outer result the same stable tie order as its page.
    for (const auto key : primary_key) {
        result_value.add_order_by(result_value.column(key, root_alias));
    }
    std::vector<std::string> order_names;
    order_names.reserve(orders.size());
    for (std::size_t i = 0; i < orders.size(); ++i) {
        auto& name = order_names.emplace_back();
        name.reserve(32);
        name.append("__ruvia_page_order_");
        append_db_number(name, static_cast<std::uint64_t>(i));
        ranked.add_select(ranked.alias(db_expression(&rank_storage, orders[i].expression_), name));
    }
    window.orders_ = std::move(orders);
    const auto window_expr = ranked.push_node(std::move(window));
    ranked.add_select(ranked.alias(window_expr, "__ruvia_page_rank"));

    db_query page;
    for (const auto& key : key_names) {
        page.add_select(page.column(key, "__ruvia_ranked"));
    }
    // Read the window ordering before the ranked query is moved into the page.
    const auto& window_orders = rank_storage.nodes_[ranked.require_expression(window_expr)].orders_;
    for (std::size_t i = 0; i < order_names.size(); ++i) {
        page.add_order_by(page.column(order_names[i], "__ruvia_ranked"), window_orders[i].direction_, window_orders[i].nulls_);
    }
    page.from(std::move(ranked), "__ruvia_ranked");
    page.where(page.binary(page.column("__ruvia_page_rank", "__ruvia_ranked"),
        db_binary_operator::equal, page.value(1)));
    // Break ordering ties using the complete primary key, including composite keys.
    for (const auto& key : key_names) {
        page.add_order_by(page.column(key, "__ruvia_ranked"));
    }
    page.limit(s.limit_ ? std::optional<query_row_count>(*s.limit_) : std::nullopt)
        .offset(s.offset_ ? std::optional<query_row_count>(*s.offset_) : std::nullopt);
    db_query included;
    const auto page_alias = root_alias == "__ruvia_page" ? "__ruvia_page_1" : "__ruvia_page";
    included.select(included.value(1)).from(std::move(page), page_alias);
    for (std::size_t i = 0; i < primary_key.size(); ++i) {
        included.and_where(included.binary(included.column(key_names[i], page_alias),
            db_binary_operator::equal, included.column(primary_key[i], root_alias)));
    }
    result_value.limit(std::nullopt).offset(std::nullopt);
    result_value.and_where(result_value.exists(std::move(included)));
    return result_value;
}

std::string db_relation_plan::unique_alias(const db_query& query, std::string_view reserved) {
    constexpr std::string_view prefix = "__ruvia_relation_";
    std::string alias;
    alias.reserve(32);
    alias.append(prefix);
    for (;;) {
        alias.resize(prefix.size());
        append_db_number(alias, static_cast<std::uint64_t>(next_alias_++));
        if (alias != reserved && !uses_source_name(query, alias)) {
            return alias;
        }
    }
}

std::string db_relation_plan::unique_projection(const db_query& query) {
    constexpr std::string_view prefix = "__ruvia_column_";
    std::string name;
    name.reserve(32);
    name.append(prefix);
    for (;;) {
        name.resize(prefix.size());
        append_db_number(name, static_cast<std::uint64_t>(next_projection_++));
        if (!uses_projection_name(query, name)) {
            return name;
        }
    }
}

void db_relation_plan::conjunction(db_query& query, db_expression& predicate, db_expression term) {
    predicate = predicate.empty() ? term : query.binary(predicate, db_binary_operator::and_value, term);
}

db_projection apply_projection(db_query& query, std::span<const db_selection> selection,
    std::string_view qualifier, bool returning, std::string_view root_table, const db_projection* previous) {
    if (selection.empty()) {
        throw std::invalid_argument("projection requires at least one field");
    }
    db_projection result;
    const auto named_count = static_cast<std::size_t>(std::ranges::count_if(selection, [](const auto& field) { return !field.is_wildcard(); }));
    result.columns_.reserve((previous ? previous->columns_.size() : 0) + named_count);
    if (previous) {
        result.columns_ = previous->columns_;
        result.wildcard_ = previous->wildcard_;
    }
    const auto alias = qualifier == root_table ? std::string_view{} : qualifier;
    const auto import_field = [&](const db_selection& field) -> db_expression {
        if (field.is_wildcard()) {
            if (!field.column_.empty()) {
                throw std::invalid_argument("wildcard projections cannot have a result alias");
            }
            result.wildcard_ = true;
            return field.expression(query, root_table, alias);
        }
        if (field.column_.empty() || std::ranges::find(result.columns_, std::string_view(field.column_)) != result.columns_.end()) {
            throw std::invalid_argument("projection requires unique nonempty field names");
        }
        result.columns_.emplace_back(field.column_);
        auto expression = !field.has_expression()
                              ? query.column(field.column_, qualifier)
                              : field.expression(query, root_table, alias);
        return query.alias(expression, field.column_);
    };
    const auto publish = [&](std::span<const db_expression> values) {
        if (returning) {
            query.returning(values);
        } else if (previous) {
            for (const auto value : values) {
                query.add_select(value);
            }
        } else {
            query.select(values);
        }
    };
    import_db_expressions(query, selection, import_field, publish);
    return result;
}

std::string format_truncate_table(std::string_view table, db_driver driver) {
    std::string sql;
    sql.reserve(17U + table.size());
    sql.append("TRUNCATE TABLE ");
    append_db_qualified_identifier(sql, table, driver);
    return sql;
}

}  // namespace ruvia::detail

// An open transaction: it owns a pooled connection until commit, rollback, or
// destruction, and a transaction abandoned by an unwinding scope must roll back
// rather than leak the connection.

namespace ruvia {

db_transaction::lease_type::lease_type(detail::db_pool_ref_type client, std::size_t slot, operation_options options) noexcept
    : client_(client),
      slot_(slot),
      options_(std::move(options)) {}

void db_transaction::lease_type::abort() noexcept {
    detail::visit_db_pool_if_present(client_, [&](auto& client) noexcept { client.abort_transaction(slot_); });
}

db_transaction::db_transaction(detail::db_pool_ref_type client, std::size_t slot, operation_options options)
    : state_(std::make_unique<operation_state_type>(lease_type{client, slot, std::move(options)})) {}

db_transaction::db_transaction(db_transaction&& other) noexcept
    : state_(std::move(other.state_)),
      io_metrics_(other.io_metrics_),
      registration_(std::move(other.registration_), this) {}

db_transaction::~db_transaction() = default;

bool db_transaction::active() const noexcept {
    return state_ != nullptr && state_->active();
}

void db_transaction::bind_operation_scope(
    ::ruvia::operation_scope& scope, detail::request_io_metrics* io_metrics) noexcept {
    io_metrics_ = io_metrics;
    registration_.bind(scope, this, &db_transaction::expire_capability);
}

void db_transaction::expire_capability(void* target) noexcept {
    auto& transaction = *static_cast<db_transaction*>(target);
    transaction.reset();
}

db_driver db_transaction::query_driver() const {
    registration_.require_active();
    return detail::db_pool_driver(state_->preparation_payload().client_);
}

// A transaction never uses the shared query cache: its reads may include its
// own uncommitted writes, which must neither be published nor be replaced by
// rows cached outside the transaction.
detail::db_query_backend db_transaction::query_backend(const lease_type& lease) const {
    return detail::db_query_backend{lease.client_, lease.slot_, io_metrics_};
}

task<db_rows> db_transaction::query_task(const db_query& query) {
    registration_.require_active();
    const auto backend = query_backend(state_->preparation_payload());
    auto plan = detail::db_query_plan::prepare(query, nullptr, backend);
    operation_guard_type operation(*state_);
    return query_plan_prepared<false>(std::move(plan), backend, std::move(operation));
}

task<std::pair<db_rows, db_rows>> db_transaction::query_and_count_task(const db_query& query, const db_query& count) {
    registration_.require_active();
    const auto backend = query_backend(state_->preparation_payload());
    auto plan = detail::db_query_plan::prepare(query, &count, backend);
    operation_guard_type operation(*state_);
    return query_plan_prepared<true>(std::move(plan), backend, std::move(operation));
}

template <bool with_count>
task<detail::db_query_result<with_count>> db_transaction::query_plan_prepared(
    detail::db_query_plan plan, detail::db_query_backend backend, operation_guard_type pending) {
    operation_guard_type operation(std::move(pending));
    operation.start();
    auto& lease_value = operation.lease();
    bool backend_failed = false;
    backend.backend_failed_ = &backend_failed;
    try {
        auto result_value = co_await detail::execute_db_query_plan<with_count>(std::move(plan), backend,
            lease_value.options_);
        operation.finish_active();
        co_return result_value;
    } catch (...) {
        // SQL failures have already retired the lease in the backend. Deadline
        // failures must retire it here while it is still owned by this operation.
        if (!backend_failed) {
            lease_value.abort();
        }
        throw;
    }
}

template <typename result_type, bool exec_statement>
task<result_type> db_transaction::run_statement(std::string sql, std::vector<db_value> params) {
    operation_guard_type operation(*state_);
    return ::ruvia::make_scoped_operation(registration_.scope(),
        statement_task<result_type, exec_statement>(
            std::move(sql), std::move(params), io_metrics_, std::move(operation)));
}

template <typename result_type, bool exec_statement>
task<result_type> db_transaction::statement_task(std::string sql, std::vector<db_value> params,
    detail::request_io_metrics* io_metrics, operation_guard_type pending) {
    detail::scoped_request_io_timer timer(io_metrics, detail::request_io_kind::db);
    operation_guard_type operation(std::move(pending));
    operation.start();
    auto& lease_value = operation.lease();
    if constexpr (exec_statement) {
        auto result_value = co_await detail::visit_db_pool(lease_value.client_, [&](auto& client) {
            return client.execute_on_transaction_slot(
                lease_value.slot_, std::move(sql), std::move(params), lease_value.options_);
        });
        operation.finish_active();
        if constexpr (!std::is_void_v<result_type>) {
            co_return result_value;
        }
    } else {
        auto rows = co_await detail::visit_db_pool(lease_value.client_, [&](auto& client) {
            return client.query_on_transaction_slot(
                lease_value.slot_, std::move(sql), std::move(params), lease_value.options_);
        });
        if constexpr (std::is_same_v<result_type, bool>) {
            try {
                if (rows.size() != 1) {
                    throw std::runtime_error("database boolean scalar query did not return one row");
                }
                const bool result = rows.front()[std::size_t{0}].template as<bool>().value();
                operation.finish_active();
                co_return result;
            } catch (...) {
                lease_value.abort();
                throw;
            }
        } else {
            operation.finish_active();
            if constexpr (!std::is_void_v<result_type>) {
                co_return rows;
            }
        }
    }
}

template <bool commit_lease>
task<void> db_transaction::finish_task(detail::request_io_metrics* io_metrics, operation_guard_type pending) {
    detail::scoped_request_io_timer timer(io_metrics, detail::request_io_kind::db);
    operation_guard_type operation(std::move(pending));
    operation.start();
    auto& lease_value = operation.lease();
    co_await detail::visit_db_pool(lease_value.client_, [&](auto& client) {
        return commit_lease
                   ? client.commit_transaction(lease_value.slot_, lease_value.options_)
                   : client.rollback_transaction(lease_value.slot_, lease_value.options_);
    });
    operation.finish_closed();
}

task<db_exec_result> db_transaction::execute_query(const db_query& query) {
    registration_.require_active();
    const auto& lease_value = state_->preparation_payload();
    auto statement = query.compile(detail::db_pool_driver(lease_value.client_));
    if (statement.returns_rows()) {
        throw std::invalid_argument("execute requires a statement without returned rows");
    }
    return run_statement<db_exec_result, true>(std::move(statement.sql_), std::move(statement.params_));
}

task<db_exec_result> db_transaction::execute_prepared(std::string sql) {
    registration_.require_active();
    return run_statement<db_exec_result, true>(std::move(sql), std::vector<db_value>{});
}

task<db_rows> db_transaction::query(
    std::string_view sql, std::span<const db_value> params) & {
    registration_.require_active();
    auto statement = prepare_db_statement(sql, params);
    return run_statement<db_rows, false>(std::move(statement.sql_), std::move(statement.params_));
}

task<db_exec_result> db_transaction::execute(
    std::string_view sql, std::span<const db_value> params) & {
    registration_.require_active();
    auto statement = prepare_db_statement(sql, params);
    return run_statement<db_exec_result, true>(std::move(statement.sql_), std::move(statement.params_));
}

task<void> db_transaction::commit() & {
    registration_.require_active();
    return ::ruvia::make_scoped_operation(
        registration_.scope(), finish_task<true>(io_metrics_, operation_guard_type(*state_)));
}

task<void> db_transaction::rollback() & {
    registration_.require_active();
    return ::ruvia::make_scoped_operation(
        registration_.scope(), finish_task<false>(io_metrics_, operation_guard_type(*state_)));
}

task<void> db_transaction::savepoint(std::string_view name) & {
    return run_identifier_command("SAVEPOINT ", name);
}

task<void> db_transaction::rollback_to_savepoint(std::string_view name) & {
    return run_identifier_command("ROLLBACK TO SAVEPOINT ", name);
}

task<void> db_transaction::release_savepoint(std::string_view name) & {
    return run_identifier_command("RELEASE SAVEPOINT ", name);
}

task<void> db_transaction::run_identifier_command(std::string_view prefix, std::string_view name) {
    registration_.require_active();
    const auto& lease_value = state_->preparation_payload();
    const auto driver = detail::db_pool_driver(lease_value.client_);
    detail::validate_db_identifier(name, driver);
    std::string sql;
    sql.reserve(prefix.size() + name.size() + 2);
    sql.append(prefix);
    detail::append_db_identifier(sql, name, driver);
    return run_statement<void, true>(std::move(sql), std::vector<db_value>{});
}

void db_transaction::require_postgresql_transaction(std::string_view feature) const {
    if (query_driver() != db_driver::postgresql) {
        throw std::invalid_argument(std::string(feature) + " requires PostgreSQL");
    }
}

task<void> db_transaction::query_void_prepared(std::string_view sql, std::span<const db_value> params) {
    registration_.require_active();
    auto statement = prepare_db_statement(sql, params);
    return run_statement<void, false>(std::move(statement.sql_), std::move(statement.params_));
}

task<bool> db_transaction::query_bool_prepared(std::string_view sql, std::span<const db_value> params) {
    registration_.require_active();
    auto statement = prepare_db_statement(sql, params);
    return run_statement<bool, false>(std::move(statement.sql_), std::move(statement.params_));
}

task<void> db_transaction::notify(std::string_view channel, std::string_view payload) & {
    require_postgresql_transaction("database notify");
    auto statement = prepare_db_notify(channel, payload);
    return run_statement<void, false>(std::move(statement.sql_), std::move(statement.params_));
}

task<void> db_transaction::advisory_lock(std::int64_t key) & {
    require_postgresql_transaction("advisory_lock");
    const std::array<db_value, 1> params{db_value(key)};
    return query_void_prepared("SELECT \"pg_advisory_xact_lock\"($1::bigint)", params);
}

task<void> db_transaction::advisory_lock(std::int32_t class_id, std::int32_t object_id) & {
    require_postgresql_transaction("advisory_lock");
    const std::array<db_value, 2> params{db_value(class_id), db_value(object_id)};
    return query_void_prepared("SELECT \"pg_advisory_xact_lock\"($1::integer, $2::integer)", params);
}

task<void> db_transaction::advisory_lock(std::string_view key, std::int64_t seed) & {
    require_postgresql_transaction("advisory_lock");
    const std::array<db_value, 2> params{db_value(key), db_value(seed)};
    return query_void_prepared("SELECT \"pg_advisory_xact_lock\"(\"hashtextextended\"($1::text, $2::bigint))", params);
}

task<bool> db_transaction::try_advisory_lock(std::int64_t key) & {
    require_postgresql_transaction("try_advisory_lock");
    const std::array<db_value, 1> params{db_value(key)};
    return query_bool_prepared("SELECT \"pg_try_advisory_xact_lock\"($1::bigint)", params);
}

task<bool> db_transaction::try_advisory_lock(std::int32_t class_id, std::int32_t object_id) & {
    require_postgresql_transaction("try_advisory_lock");
    const std::array<db_value, 2> params{db_value(class_id), db_value(object_id)};
    return query_bool_prepared("SELECT \"pg_try_advisory_xact_lock\"($1::integer, $2::integer)", params);
}

task<bool> db_transaction::try_advisory_lock(std::string_view key, std::int64_t seed) & {
    require_postgresql_transaction("try_advisory_lock");
    const std::array<db_value, 2> params{db_value(key), db_value(seed)};
    return query_bool_prepared("SELECT \"pg_try_advisory_xact_lock\"(\"hashtextextended\"($1::text, $2::bigint))", params);
}

task<void> db_transaction::declare_cursor(
    std::string_view name, std::string_view sql, std::span<const db_value> params, bool scroll) & {
    require_postgresql_transaction("declare_cursor");
    validate_db_sql_text(sql);
    detail::validate_db_identifier(name, db_driver::postgresql);
    const std::string_view mode = scroll ? " SCROLL CURSOR FOR " : " NO SCROLL CURSOR FOR ";
    std::string command;
    command.reserve(8 + name.size() + 2 + mode.size() + sql.size());
    command.append("DECLARE ");
    detail::append_db_identifier(command, name, db_driver::postgresql);
    command.append(mode);
    command.append(sql);
    auto owned_params = detail::clone_db_values(params);
    return run_statement<void, true>(std::move(command), std::move(owned_params));
}

task<db_rows> db_transaction::fetch_cursor(std::string_view name, std::uint64_t count) & {
    require_postgresql_transaction("fetch_cursor");
    if (count > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())) {
        throw std::invalid_argument("cursor fetch count exceeds signed 64-bit range");
    }
    detail::validate_db_identifier(name, db_driver::postgresql);
    std::string sql;
    sql.reserve(14 + 20 + 6 + name.size() + 2);
    sql.append("FETCH FORWARD ");
    detail::append_db_number(sql, count);
    sql.append(" FROM ");
    detail::append_db_identifier(sql, name, db_driver::postgresql);
    return run_statement<db_rows, false>(std::move(sql), std::vector<db_value>{});
}

task<void> db_transaction::close_cursor(std::string_view name) & {
    require_postgresql_transaction("close_cursor");
    return run_identifier_command("CLOSE ", name);
}

void db_transaction::reset() noexcept {
    if (state_ != nullptr) {
        state_->reset();
    }
}

}  // namespace ruvia

#ifdef RUVIA_ENABLE_REDIS
#endif

namespace ruvia {

void validate_db_sql_text(std::string_view sql) {
    if (!detail::has_sql_non_whitespace(sql)) {
        throw std::invalid_argument("SQL must not be empty");
    }
    if (sql.find('\0') != std::string_view::npos) {
        throw std::invalid_argument("SQL must not contain NUL bytes");
    }
}

prepared_db_statement prepare_db_statement(
    std::string_view sql, std::span<const db_value> params) {
    validate_db_sql_text(sql);
    return prepared_db_statement{std::string(sql), detail::clone_db_values(params)};
}

prepared_db_statement prepare_db_notify(
    std::string_view channel, std::string_view payload) {
    if (channel.empty()) {
        throw std::invalid_argument("database notify channel must not be empty");
    }
    const std::array<db_value, 2> params{db_value(channel), db_value(payload)};
    return prepare_db_statement("SELECT \"pg_notify\"($1::text, $2::text)", params);
}

namespace detail {
namespace {

[[noreturn]] void throw_db_plan_timeout(db_driver driver) {
    throw db_error(db_error::code_type::timeout, driver, "database query timed out");
}

}  // namespace

db_query_plan db_query_plan::prepare(const db_query& query, const db_query* count, const db_query_backend& backend) {
    const auto driver = db_pool_driver(backend.pool_);
    const auto prepare_step = [&](const db_query& input) {
        auto statement = input.compile(driver);
        if (!statement.returns_rows()) {
            throw std::invalid_argument(count ? "query and count require statements that return rows"
                                              : "query requires a statement that returns rows");
        }
#ifdef RUVIA_ENABLE_REDIS
        std::optional<std::string> key;
        if (backend.cache_ != nullptr) {
            key = backend.cache_->key(input, statement, driver);
        }
        return db_query_step{std::move(statement.sql_), std::move(statement.params_), std::move(key),
            input.cache_duration()};
#else
        return db_query_step{std::move(statement.sql_), std::move(statement.params_)};
#endif
    };
    db_query_plan plan{prepare_step(query), std::nullopt};
    if (count != nullptr) {
        plan.second_.emplace(prepare_step(*count));
    }
    return plan;
}

task<db_rows> query_db_backend(const db_query_backend& backend, std::string sql,
    std::vector<db_value> params, operation_options& options) {
    return visit_db_pool(backend.pool_, [&](auto& pool) {
        if (backend.slot_.has_value()) {
            return pool.query_on_transaction_slot(
                *backend.slot_, std::move(sql), std::move(params), options);
        }
        return pool.query(std::move(sql), std::move(params), std::move(options), backend.io_metrics_);
    });
}

// One deadline and one execution chain for direct, cached, and mixed sequences.
// A statement without a cache key goes straight to the pool.
template <bool with_count>
task<db_query_result<with_count>> execute_db_query_plan(
    db_query_plan plan, db_query_backend backend, operation_options options) {
    const auto driver = db_pool_driver(backend.pool_);
    const operation_deadline deadline_value(options.timeout_);
    std::array<std::optional<db_rows>, with_count ? 2 : 1> results;
    for (std::size_t index = 0; index < results.size(); ++index) {
        auto& step = index == 0 ? plan.first_ : plan.second_.value();
        if (deadline_value.expired()) {
            throw_db_plan_timeout(driver);
        }
        auto step_options = db_remaining_options(options, deadline_value);
#ifdef RUVIA_ENABLE_REDIS
        if (backend.cache_ != nullptr && step.cache_key_.has_value()) {
            // The cache runs only on pooled backends; a transaction never
            // carries one.
            results[index].emplace(co_await backend.cache_->wrap(step.cache_duration_,
                std::move(*step.cache_key_),
                db_cache_query(backend, std::move(step.sql_), std::move(step.params_)), std::move(step_options),
                deadline_value));
        } else
#endif
        {
            scoped_request_io_timer timer(
                backend.slot_.has_value() ? backend.io_metrics_ : nullptr, request_io_kind::db);
            try {
                results[index].emplace(co_await query_db_backend(
                    backend, std::move(step.sql_), std::move(step.params_), step_options));
            } catch (...) {
                if (backend.backend_failed_ != nullptr) {
                    *backend.backend_failed_ = true;
                }
                throw;
            }
        }
        if (deadline_value.expired()) {
            throw_db_plan_timeout(driver);
        }
    }
    if constexpr (with_count) {
        co_return std::pair{std::move(*results[0]), std::move(*results[1])};
    } else {
        co_return std::move(*results[0]);
    }
}

template task<db_query_result<false>> execute_db_query_plan<false>(
    db_query_plan plan, db_query_backend backend, operation_options options);
template task<db_query_result<true>> execute_db_query_plan<true>(
    db_query_plan plan, db_query_backend backend, operation_options options);

}  // namespace detail
}  // namespace ruvia

// A streaming query result: the rows arrive one at a time from a pooled
// connection the result holds for as long as it is active, and every exit --
// exhaustion, explicit close, failure, or destruction -- must return that
// connection exactly once.

namespace ruvia {
db_stream_result::lease_type::lease_type(detail::db_pool_ref_type client, std::size_t slot, operation_options options) noexcept
    : client_(client),
      slot_(slot),
      options_(std::move(options)) {}

void db_stream_result::lease_type::abort() noexcept {
    detail::visit_db_pool_if_present(client_, [&](auto& client) noexcept { client.abort_stream(slot_); });
}

db_stream_result::state_type::state_type(
    lease_type lease, std::vector<std::string> column_names) noexcept
    : column_names_(std::move(column_names)),
      operation_(std::move(lease)) {}

db_stream_result::db_stream_result(detail::db_pool_ref_type client, std::size_t slot,
    std::vector<std::string> column_names,
    operation_options options)
    : state_(std::make_unique<state_type>(lease_type{client, slot, std::move(options)}, std::move(column_names))) {}

db_stream_result::db_stream_result(db_stream_result&& other) noexcept
    : state_(std::move(other.state_)),
      io_metrics_(other.io_metrics_),
      registration_(std::move(other.registration_), this) {}

db_stream_result::~db_stream_result() = default;

bool db_stream_result::active() const noexcept {
    return state_ != nullptr && state_->operation_.active();
}

std::span<const std::string> db_stream_result::column_names() const& {
    registration_.require_active();
    return state_ != nullptr ? std::span<const std::string>(state_->column_names_)
                             : std::span<const std::string>{};
}

void db_stream_result::bind_operation_scope(
    ::ruvia::operation_scope& scope, detail::request_io_metrics* io_metrics) noexcept {
    io_metrics_ = io_metrics;
    registration_.bind(scope, this, &db_stream_result::expire_capability);
}

void db_stream_result::expire_capability(void* target) noexcept {
    auto& result_value = *static_cast<db_stream_result*>(target);
    result_value.reset();
}

namespace {

// The stream's handle options with this operation's own: its timeout only
// tightens, a supplied token replaces the inherited one. An already-stopped
// token gives the lease back before it reaches the connection, so the stream
// stays usable.
template <typename guard_type>
[[nodiscard]] operation_options admit_stream_operation(guard_type& operation, operation_options operation_value) {
    auto effective = override_operation_options(operation.lease().options_, std::move(operation_value));
    if (effective.stop_token_.stop_requested()) {
        operation.finish_active();
        throw db_error(db_error::code_type::cancelled, std::nullopt, "database stream operation cancelled");
    }
    return effective;
}

}  // namespace

task<std::optional<db_row>> db_stream_result::read(operation_options operation) & {
    registration_.require_active();
    return ::ruvia::make_scoped_operation(registration_.scope(),
        read_task(io_metrics_, operation_guard_type(state_->operation_), state_->column_names_, std::move(operation)));
}

task<std::optional<db_row>> db_stream_result::read_task(
    detail::request_io_metrics* io_metrics, operation_guard_type pending,
    std::vector<std::string>& column_names, operation_options operation_value) {
    detail::scoped_request_io_timer timer(io_metrics, detail::request_io_kind::db);
    operation_guard_type operation(std::move(pending));
    operation.start();
    auto effective = admit_stream_operation(operation, std::move(operation_value));
    auto& lease_value = operation.lease();
    auto row = co_await detail::visit_db_pool(lease_value.client_, [&](auto& client) {
        return client.read_stream_row(lease_value.slot_, lease_value.fields_, column_names, effective);
    });
    if (row) {
        operation.finish_active();
    } else {
        operation.finish_closed();
    }
    co_return row;
}

task<void> db_stream_result::close(operation_options operation) & {
    registration_.require_active();
    return ::ruvia::make_scoped_operation(
        registration_.scope(), close_task(io_metrics_, operation_guard_type(state_->operation_), std::move(operation)));
}

task<void> db_stream_result::close_task(
    detail::request_io_metrics* io_metrics, operation_guard_type pending, operation_options operation_value) {
    detail::scoped_request_io_timer timer(io_metrics, detail::request_io_kind::db);
    operation_guard_type operation(std::move(pending));
    if (operation.finished()) {
        co_return;
    }
    operation.start();
    auto effective = admit_stream_operation(operation, std::move(operation_value));
    auto& lease_value = operation.lease();
    co_await detail::visit_db_pool(
        lease_value.client_, [&](auto& client) { return client.close_stream(lease_value.slot_, effective); });
    operation.finish_closed();
}

void db_stream_result::reset() noexcept {
    if (state_ != nullptr) {
        state_->operation_.reset();
    }
}

}  // namespace ruvia

namespace ruvia {
namespace {
using detail::append_db_identifier;
using detail::append_db_qualified_identifier;
using detail::append_db_string_literal;
std::string_view action(db_referential_action a) {
    switch (a) {
        case db_referential_action::no_action:
            return "NO ACTION";
        case db_referential_action::restrict:
            return "RESTRICT";
        case db_referential_action::cascade:
            return "CASCADE";
        case db_referential_action::set_null:
            return "SET NULL";
        case db_referential_action::set_default:
            return "SET DEFAULT";
    }
    throw std::invalid_argument("unknown referential action");
}
std::string_view method(db_index_method m) {
    switch (m) {
        case db_index_method::btree:
            return "btree";
        case db_index_method::hash:
            return "hash";
        case db_index_method::gin:
            return "gin";
        case db_index_method::gist:
            return "gist";
        case db_index_method::sp_gist:
            return "spgist";
        case db_index_method::brin:
            return "brin";
    }
    throw std::invalid_argument("unknown index method");
}
std::string_view function_language_name(db_function_language language) {
    switch (language) {
        case db_function_language::plpgsql:
            return "plpgsql";
        case db_function_language::sql:
            return "sql";
    }
    throw std::invalid_argument("unknown function language");
}
std::string_view function_volatility_clause(db_function_volatility volatility) {
    switch (volatility) {
        case db_function_volatility::volatile_value:
            return {};
        case db_function_volatility::stable:
            return " STABLE";
        case db_function_volatility::immutable:
            return " IMMUTABLE";
    }
    throw std::invalid_argument("unknown function volatility");
}
std::string_view trigger_timing_clause(db_trigger_timing timing) {
    switch (timing) {
        case db_trigger_timing::before:
            return " BEFORE ";
        case db_trigger_timing::after:
            return " AFTER ";
        case db_trigger_timing::instead_of:
            return " INSTEAD OF ";
    }
    throw std::invalid_argument("unknown trigger timing");
}
std::string_view trigger_event_name(db_trigger_event event) {
    switch (event) {
        case db_trigger_event::insert:
            return "INSERT";
        case db_trigger_event::update:
            return "UPDATE";
        case db_trigger_event::delete_value:
            return "DELETE";
        case db_trigger_event::truncate:
            return "TRUNCATE";
    }
    throw std::invalid_argument("unknown trigger event");
}
std::string begin_alter_table(
    db_driver driver, std::string_view table_value, std::size_t extra_reserve) {
    std::string sql;
    sql.reserve(extra_reserve + table_value.size());
    sql += "ALTER TABLE ";
    append_db_qualified_identifier(sql, table_value, driver);
    return sql;
}

std::string begin_alter_column(db_driver driver,
    std::string_view table_value, std::string_view column_value, std::string_view clause, std::size_t extra_reserve) {
    auto sql = begin_alter_table(driver, table_value, extra_reserve + column_value.size());
    sql += clause;
    append_db_identifier(sql, column_value, driver);
    return sql;
}

std::string begin_timescale_perform(db_driver driver,
    std::string_view function, std::string_view table_value, std::size_t extra_reserve) {
    std::string name;
    name.reserve(table_value.size() + 4U);
    append_db_qualified_identifier(name, table_value, driver);
    std::string sql;
    sql.reserve(extra_reserve + name.size());
    sql += "PERFORM \"";
    sql += function;
    sql += "\"(";
    append_db_string_literal(sql, name, driver);
    return sql;
}

std::string make_drop_object_sql(db_driver driver,
    std::string_view kind, std::string_view name, bool qualified, bool if_exists) {
    std::string sql;
    sql.reserve(48U + name.size());
    sql += "DROP ";
    sql += kind;
    sql += ' ';
    if (if_exists) {
        sql += "IF EXISTS ";
    }
    if (qualified) {
        append_db_qualified_identifier(sql, name, driver);
    } else {
        append_db_identifier(sql, name, driver);
    }
    return sql;
}

std::string make_add_timescale_policy_sql(db_driver driver,
    std::string_view function, std::string_view table_value, std::string_view param_name, db_expression after,
    bool if_not_exists) {
    if (after.empty()) {
        throw std::invalid_argument("schema expression is required");
    }
    auto sql = begin_timescale_perform(driver, function, table_value, 96U);
    sql += ", \"";
    sql += param_name;
    sql += "\" => ";
    detail::append_db_expression(sql, after, driver);
    sql += ", \"if_not_exists\" => ";
    sql += if_not_exists ? "TRUE" : "FALSE";
    sql += ')';
    return sql;
}

std::string make_remove_timescale_policy_sql(db_driver driver,
    std::string_view function, std::string_view table_value, bool if_exists) {
    auto sql = begin_timescale_perform(driver, function, table_value, 72U);
    sql += ", \"if_exists\" => ";
    sql += if_exists ? "TRUE" : "FALSE";
    sql += ')';
    return sql;
}
}  // namespace

db_schema::db_schema(db_schema_options options)
    : driver_(options.driver_) {
    detail::require_db_dialect(driver_);
}
void db_schema::append(std::string sql, db_migration_atomicity a, bool procedural) {
    statements_.push_back(statement_type{std::move(sql), a, procedural});
}
void db_schema::require_postgresql() const {
    if (driver_ != db_driver::postgresql) {
        throw std::invalid_argument("schema operation requires PostgreSQL");
    }
}
// MariaDB accepts RESTRICT and CASCADE on drops only for porting and ignores
// them, so a requested CASCADE would silently leave dependents in place.
void db_schema::append_drop_behavior(std::string& s, db_drop_behavior b) const {
    switch (b) {
        case db_drop_behavior::restrict:
            return;
        case db_drop_behavior::cascade:
            if (driver_ == db_driver::mariadb) {
                throw std::invalid_argument("MariaDB drops do not support CASCADE");
            }
            s += " CASCADE";
            return;
    }
    throw std::invalid_argument("unknown drop behavior");
}
// Entity constraints follow PostgreSQL's default <table>_<name>_<suffix> naming
// on the unqualified table name.
std::string db_schema::constraint_name(std::string_view table, std::string_view relation, std::string_view suffix) const {
    const auto local = detail::unqualified_db_name(table);
    std::string name;
    name.reserve(local.size() + relation.size() + suffix.size() + 2U);
    name.append(local);
    if (!relation.empty()) {
        name.push_back('_');
        name.append(relation);
    }
    name.push_back('_');
    name.append(suffix);
    return name;
}
void db_schema::append_expression(std::string& s, db_expression e) const {
    if (e.empty()) {
        throw std::invalid_argument("schema expression is required");
    }
    detail::append_db_expression(s, e, driver_);
}
void db_schema::append_type(std::string& s, const db_type_definition& t) const {
    detail::append_db_type_name(s, t.data_type_, t.custom_name_, t.length_, t.precision_, t.scale_,
        t.array_, driver_);
}
void db_schema::append_column(std::string& s, const db_schema_column& c) const {
    if (c.generated_type_ != db_generated_type::none && c.generated_type_ != db_generated_type::stored &&
        c.generated_type_ != db_generated_type::virtual_value) {
        throw std::invalid_argument("unsupported generated column storage mode");
    }
    if (c.name_.empty()) {
        throw std::invalid_argument("column name is required");
    }
    if (c.generated_type_ == db_generated_type::none && !c.as_expression_.empty()) {
        throw std::invalid_argument("generated column expression requires generated metadata");
    }
    if (c.generated_type_ != db_generated_type::none && (c.identity_ != db_identity::none || !c.default_value_.empty())) {
        throw std::invalid_argument("generated column conflicts with identity or default");
    }
    append_db_identifier(s, c.name_, driver_);
    s += ' ';
    append_type(s, c.type_);
    if (c.generated_type_ != db_generated_type::none) {
        if (c.as_expression_.empty()) {
            throw std::invalid_argument("generated column expression is required");
        }
        if (driver_ == db_driver::postgresql && c.generated_type_ == db_generated_type::virtual_value) {
            throw std::invalid_argument("PostgreSQL schema compilation supports stored generated columns only");
        }
        if (driver_ == db_driver::mariadb && (!c.nullable_ || c.primary_key_)) {
            throw std::invalid_argument("MariaDB generated columns cannot declare NOT NULL or PRIMARY KEY");
        }
        s += " GENERATED ALWAYS AS (";
        append_expression(s, c.as_expression_);
        s += ")";
        if (driver_ == db_driver::postgresql || c.generated_type_ == db_generated_type::stored) {
            s += " STORED";
        } else {
            s += " VIRTUAL";
        }
    }
    if (c.identity_ != db_identity::none) {
        if (!c.default_value_.empty() || c.nullable_ || c.type_.array_ ||
            (c.type_.data_type_ != db_data_type::small_int && c.type_.data_type_ != db_data_type::integer && c.type_.data_type_ != db_data_type::big_int)) {
            throw std::invalid_argument("identity requires a non-null integer column without a default");
        }
        if (driver_ == db_driver::postgresql) {
            s += " GENERATED ";
            s += (c.identity_ == db_identity::always ? "ALWAYS" : "BY DEFAULT");
            s += " AS IDENTITY";
        } else {
            if (c.identity_ == db_identity::always) {
                throw std::invalid_argument("MariaDB cannot enforce GENERATED ALWAYS identity semantics");
            }
            s += " AUTO_INCREMENT";
        }
    }
    if (!c.nullable_) {
        s += " NOT NULL";
    }
    if (c.unique_) {
        s += " UNIQUE";
    }
    if (c.primary_key_) {
        s += " PRIMARY KEY";
    }
    if (!c.default_value_.empty()) {
        s += " DEFAULT ";
        append_expression(s, c.default_value_);
    }
}
void db_schema::append_constraint(std::string& s, const db_schema_constraint& c) const {
    if ((c.deferrable_ || c.initially_deferred_ || c.not_valid_) && driver_ != db_driver::postgresql) {
        throw std::invalid_argument("deferred and NOT VALID constraints require PostgreSQL");
    }
    if (c.initially_deferred_ && !c.deferrable_) {
        throw std::invalid_argument("INITIALLY DEFERRED requires DEFERRABLE");
    }
    if (c.kind_ == db_constraint_kind::check && c.deferrable_) {
        throw std::invalid_argument("CHECK constraints cannot be deferred");
    }
    if (c.not_valid_ && c.kind_ != db_constraint_kind::check && c.kind_ != db_constraint_kind::foreign_key) {
        throw std::invalid_argument("NOT VALID requires a CHECK or foreign key constraint");
    }
    if (c.kind_ != db_constraint_kind::check && c.columns_.empty()) {
        throw std::invalid_argument("constraint requires columns");
    }
    if (c.kind_ == db_constraint_kind::foreign_key && c.columns_.size() != c.referenced_columns_.size()) {
        throw std::invalid_argument("foreign key column counts differ");
    }
    if (c.name_.empty()) {
        throw std::invalid_argument("constraint name is required");
    }
    append_db_identifier(s, c.name_, driver_);
    s += ' ';
    if (c.kind_ == db_constraint_kind::check) {
        s += "CHECK (";
        append_expression(s, c.check_);
        s += ')';
    } else if (c.kind_ == db_constraint_kind::primary_key || c.kind_ == db_constraint_kind::unique) {
        s += (c.kind_ == db_constraint_kind::primary_key ? "PRIMARY KEY (" : "UNIQUE (");
        for (size_t i = 0; i < c.columns_.size(); ++i) {
            if (i) {
                s += ',';
            }
            append_db_identifier(s, c.columns_[i], driver_);
        }
        s += ')';
    } else {
        if (c.columns_.empty() || c.referenced_table_.empty() || c.referenced_columns_.empty()) {
            throw std::invalid_argument("foreign key fields are required");
        }
        s += "FOREIGN KEY (";
        for (size_t i = 0; i < c.columns_.size(); ++i) {
            if (i) {
                s += ',';
            }
            append_db_identifier(s, c.columns_[i], driver_);
        }
        s += ") REFERENCES ";
        append_db_qualified_identifier(s, c.referenced_table_, driver_);
        s += " (";
        for (size_t i = 0; i < c.referenced_columns_.size(); ++i) {
            if (i) {
                s += ',';
            }
            append_db_identifier(s, c.referenced_columns_[i], driver_);
        }
        s += ") ON DELETE ";
        s += action(c.on_delete_);
        s += " ON UPDATE ";
        s += action(c.on_update_);
    }
    if (c.deferrable_) {
        s += " DEFERRABLE";
    }
    if (c.initially_deferred_) {
        s += " INITIALLY DEFERRED";
    }
    if (c.not_valid_) {
        s += " NOT VALID";
    }
}

void db_schema::create_schema(std::string_view n, bool ine) {
    std::string s;
    s.reserve(32U + n.size());
    s += "CREATE SCHEMA ";
    if (ine) {
        s += "IF NOT EXISTS ";
    }
    append_db_identifier(s, n, driver_);
    append(std::move(s));
}
void db_schema::drop_schema(std::string_view n, db_drop_behavior b, bool ie) {
    auto s = make_drop_object_sql(driver_, "SCHEMA", n, false, ie);
    append_drop_behavior(s, b);
    append(std::move(s));
}
void db_schema::create_table(const db_table_definition& t) {
    if (t.name_.empty() || t.columns_.empty()) {
        throw std::invalid_argument("table and columns are required");
    }
    if (t.temporary_ && t.unlogged_) {
        throw std::invalid_argument("table cannot be both temporary and unlogged");
    }
    if (t.unlogged_) {
        require_postgresql();
    }
    std::size_t primary_columns = 0;
    for (std::size_t i = 0; i < t.columns_.size(); ++i) {
        primary_columns += t.columns_[i].primary_key_ ? 1 : 0;
        for (std::size_t j = 0; j < i; ++j) {
            if (t.columns_[i].name_ == t.columns_[j].name_) {
                throw std::invalid_argument("duplicate table column");
            }
        }
    }
    if (primary_columns > 1) {
        throw std::invalid_argument("composite primary keys require a table constraint");
    }
    for (const auto& constraint : t.constraints_) {
        if (constraint.not_valid_) {
            throw std::invalid_argument("NOT VALID is only supported when adding a constraint");
        }
        if (constraint.kind_ == db_constraint_kind::primary_key && primary_columns++ != 0) {
            throw std::invalid_argument("duplicate table primary key");
        }
        if (driver_ == db_driver::mariadb && constraint.kind_ == db_constraint_kind::primary_key) {
            for (const auto& column : t.columns_) {
                if (column.generated_type_ != db_generated_type::none &&
                    std::ranges::find(constraint.columns_, column.name_) != constraint.columns_.end()) {
                    throw std::invalid_argument("MariaDB generated columns cannot be primary keys");
                }
            }
        }
    }
    std::string s;
    s.reserve(32U + t.name_.size() + t.columns_.size() * 32U + t.constraints_.size() * 48U);
    s += "CREATE ";
    if (t.temporary_) {
        s += "TEMPORARY ";
    }
    if (t.unlogged_) {
        s += "UNLOGGED ";
    }
    s += "TABLE ";
    if (t.if_not_exists_) {
        s += "IF NOT EXISTS ";
    }
    append_db_qualified_identifier(s, t.name_, driver_);
    s += " (";
    for (size_t i = 0; i < t.columns_.size(); ++i) {
        if (i) {
            s += ", ";
        }
        append_column(s, t.columns_[i]);
    }
    for (auto& c : t.constraints_) {
        s += ", CONSTRAINT ";
        append_constraint(s, c);
    }
    s += ')';
    append(std::move(s));
}
void db_schema::drop_table(std::string_view t, db_drop_behavior b, bool ie) {
    auto s = make_drop_object_sql(driver_, "TABLE", t, true, ie);
    append_drop_behavior(s, b);
    append(std::move(s));
}
void db_schema::rename_table(std::string_view t, std::string_view n) {
    auto s = begin_alter_table(driver_, t, 32U + t.size() + n.size());
    s += " RENAME TO ";
    if (driver_ == db_driver::mariadb) {
        if (const auto dot = t.rfind('.'); dot != std::string_view::npos) {
            append_db_qualified_identifier(s, t.substr(0, dot), driver_);
            s += '.';
        }
    }
    append_db_identifier(s, n, driver_);
    append(std::move(s));
}
void db_schema::add_column(std::string_view t, const db_schema_column& c, bool ie) {
    auto s = begin_alter_table(driver_, t, 64U + c.name_.size());
    s += " ADD COLUMN ";
    if (ie) {
        s += "IF NOT EXISTS ";
    }
    append_column(s, c);
    append(std::move(s));
}
void db_schema::drop_column(std::string_view t, std::string_view c, db_drop_behavior b, bool ie) {
    auto s = begin_alter_table(driver_, t, 48U + c.size());
    s += " DROP COLUMN ";
    if (ie) {
        s += "IF EXISTS ";
    }
    append_db_identifier(s, c, driver_);
    append_drop_behavior(s, b);
    append(std::move(s));
}
void db_schema::rename_column(std::string_view t, std::string_view c, std::string_view n) {
    auto s = begin_alter_column(driver_, t, c, " RENAME COLUMN ", 40U + n.size());
    s += " TO ";
    append_db_identifier(s, n, driver_);
    append(std::move(s));
}
void db_schema::alter_column_type(std::string_view t, std::string_view c, const db_type_definition& ty, db_expression u) {
    require_postgresql();
    auto s = begin_alter_column(driver_, t, c, " ALTER COLUMN ", 64U);
    s += " TYPE ";
    append_type(s, ty);
    if (!u.empty()) {
        s += " USING ";
        append_expression(s, u);
    }
    append(std::move(s));
}
void db_schema::set_column_nullable(std::string_view t, std::string_view c, bool n) {
    require_postgresql();
    auto s = begin_alter_column(driver_, t, c, " ALTER COLUMN ", 48U);
    s += n ? " DROP NOT NULL" : " SET NOT NULL";
    append(std::move(s));
}
void db_schema::set_column_default(std::string_view t, std::string_view c, db_expression e) {
    auto s = begin_alter_column(driver_, t, c, " ALTER COLUMN ", 64U);
    s += " SET DEFAULT ";
    append_expression(s, e);
    append(std::move(s));
}
void db_schema::drop_column_default(std::string_view t, std::string_view c) {
    auto s = begin_alter_column(driver_, t, c, " ALTER COLUMN ", 48U);
    s += " DROP DEFAULT";
    append(std::move(s));
}
void db_schema::add_constraint(std::string_view t, const db_schema_constraint& c) {
    auto s = begin_alter_table(driver_, t, 64U + c.name_.size());
    s += " ADD CONSTRAINT ";
    append_constraint(s, c);
    append(std::move(s));
}
void db_schema::drop_constraint(std::string_view t, std::string_view n, db_drop_behavior b, bool ie) {
    auto s = begin_alter_table(driver_, t, 56U + n.size());
    s += " DROP CONSTRAINT ";
    if (ie) {
        s += "IF EXISTS ";
    }
    append_db_identifier(s, n, driver_);
    append_drop_behavior(s, b);
    append(std::move(s));
}
void db_schema::validate_constraint(std::string_view t, std::string_view n) {
    require_postgresql();
    auto s = begin_alter_column(driver_, t, n, " VALIDATE CONSTRAINT ", 48U);
    append(std::move(s));
}
void db_schema::create_index(const db_index_definition& i) {
    if (driver_ != db_driver::postgresql && (i.concurrently_ || i.method_ != db_index_method::btree || !i.include_.empty() || !i.where_.empty())) {
        throw std::invalid_argument("selected index options require PostgreSQL");
    }
    if (i.name_.empty() || i.table_.empty() || i.keys_.empty()) {
        throw std::invalid_argument("index name, table and keys are required");
    }
    std::string s;
    s.reserve(64U + i.name_.size() + i.table_.size() + i.keys_.size() * 24U + i.include_.size() * 16U);
    s += "CREATE ";
    if (i.unique_) {
        s += "UNIQUE ";
    }
    s += "INDEX ";
    if (i.concurrently_) {
        s += "CONCURRENTLY ";
    }
    if (i.if_not_exists_) {
        s += "IF NOT EXISTS ";
    }
    append_db_identifier(s, i.name_, driver_);
    if (driver_ == db_driver::mariadb) {
        s += " USING ";
        s += method(i.method_);
    }
    s += " ON ";
    append_db_qualified_identifier(s, i.table_, driver_);
    if (driver_ == db_driver::postgresql) {
        s += " USING ";
        s += method(i.method_);
    }
    s += " (";
    for (size_t n = 0; n < i.keys_.size(); ++n) {
        if (n) {
            s += ", ";
        }
        const auto& key = i.keys_[n];
        if (key.column_.empty() == key.expression_.empty()) {
            throw std::invalid_argument("an index key requires exactly one column or expression");
        }
        if (driver_ != db_driver::postgresql && (!key.expression_.empty() || !key.operator_class_.empty() || key.nulls_ != db_nulls_order::default_value)) {
            throw std::invalid_argument("selected index key options require PostgreSQL");
        }
        if (!i.keys_[n].column_.empty()) {
            append_db_identifier(s, i.keys_[n].column_, driver_);
        } else {
            s += '(';
            append_expression(s, i.keys_[n].expression_);
            s += ')';
        }
        if (!i.keys_[n].operator_class_.empty()) {
            s += ' ';
            append_db_qualified_identifier(s, i.keys_[n].operator_class_, driver_);
        }
        if (i.keys_[n].order_ == db_order_direction::desc) {
            s += " DESC";
        }
        if (i.keys_[n].nulls_ == db_nulls_order::first) {
            s += " NULLS FIRST";
        }
        if (i.keys_[n].nulls_ == db_nulls_order::last) {
            s += " NULLS LAST";
        }
    }
    s += ')';
    if (!i.include_.empty()) {
        s += " INCLUDE (";
        for (size_t n = 0; n < i.include_.size(); ++n) {
            if (n) {
                s += ',';
            }
            append_db_identifier(s, i.include_[n], driver_);
        }
        s += ')';
    }
    if (!i.where_.empty()) {
        s += " WHERE ";
        append_expression(s, i.where_);
    }
    append(std::move(s), i.concurrently_ ? db_migration_atomicity::unwrapped : db_migration_atomicity::transactional);
}
void db_schema::drop_index(std::string_view n, bool c, bool ie) {
    require_postgresql();
    std::string s;
    s.reserve(40U + n.size());
    s += "DROP INDEX ";
    if (c) {
        s += "CONCURRENTLY ";
    }
    if (ie) {
        s += "IF EXISTS ";
    }
    append_db_qualified_identifier(s, n, driver_);
    append(std::move(s), c ? db_migration_atomicity::unwrapped : db_migration_atomicity::transactional);
}
void db_schema::create_enum(std::string_view n, std::span<const std::string_view> v) {
    require_postgresql();
    if (v.empty()) {
        throw std::invalid_argument("enum values are required");
    }
    std::string s;
    s.reserve(32U + n.size() + v.size() * 12U);
    s += "CREATE TYPE ";
    append_db_qualified_identifier(s, n, driver_);
    s += " AS ENUM (";
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) {
            s += ',';
        }
        append_db_string_literal(s, v[i], driver_);
    }
    s += ')';
    append(std::move(s));
}
void db_schema::add_enum_value(std::string_view n, std::string_view v, bool ie) {
    require_postgresql();
    std::string s;
    s.reserve(48U + n.size() + v.size());
    s += "ALTER TYPE ";
    append_db_qualified_identifier(s, n, driver_);
    s += " ADD VALUE ";
    if (ie) {
        s += "IF NOT EXISTS ";
    }
    append_db_string_literal(s, v, driver_);
    append(std::move(s), db_migration_atomicity::unwrapped);
}
void db_schema::drop_enum(std::string_view n, db_drop_behavior b, bool ie) {
    require_postgresql();
    auto s = make_drop_object_sql(driver_, "TYPE", n, true, ie);
    append_drop_behavior(s, b);
    append(std::move(s));
}
void db_schema::create_extension(std::string_view n, bool ie) {
    require_postgresql();
    std::string s;
    s.reserve(36U + n.size());
    s += "CREATE EXTENSION ";
    if (ie) {
        s += "IF NOT EXISTS ";
    }
    append_db_identifier(s, n, driver_);
    append(std::move(s));
}
void db_schema::create_view(std::string_view n, const db_query& q, bool replace, bool mat) {
    q.require_select_query();
    if (mat) {
        require_postgresql();
    }
    if (replace && mat) {
        throw std::invalid_argument("materialized views cannot use OR REPLACE");
    }
    auto st = q.compile(driver_, db_parameter_mode::literal);
    std::string s;
    s.reserve(32U + n.size() + st.sql().size());
    s += "CREATE ";
    if (replace) {
        s += "OR REPLACE ";
    }
    if (mat) {
        s += "MATERIALIZED ";
    }
    s += "VIEW ";
    append_db_qualified_identifier(s, n, driver_);
    s += " AS ";
    s += st.sql();
    append(std::move(s));
}
void db_schema::drop_view(std::string_view n, bool mat, db_drop_behavior b, bool ie) {
    if (mat) {
        require_postgresql();
    }
    auto s = make_drop_object_sql(driver_, mat ? "MATERIALIZED VIEW" : "VIEW", n, true, ie);
    append_drop_behavior(s, b);
    append(std::move(s));
}
void db_schema::execute(const db_query& q) {
    if (q.returns_rows()) {
        throw std::invalid_argument("schema execute requires a command without returned rows");
    }
    auto st = q.compile(driver_, db_parameter_mode::literal);
    append(std::string(st.sql()));
}
void db_schema::perform(db_expression e) {
    require_postgresql();
    std::string s;
    s.reserve(64U);
    detail::append_db_perform_statement(s, e);
    append(std::move(s), db_migration_atomicity::transactional, true);
}

void db_schema::run(const db_procedure& procedure) {
    require_postgresql();
    auto body = procedure.body();
    std::string sql;
    sql.reserve(3U + body.size() + 18U);
    sql += "DO ";
    detail::append_db_dollar_body(sql, body);
    append(std::move(sql));
}
void db_schema::create_trigger_function(std::string_view name, const db_procedure& procedure, bool replace) {
    require_postgresql();
    auto body = procedure.body();
    std::string sql;
    sql.reserve((replace ? 27U : 16U) + name.size() + 44U + body.size() + 16U);
    sql += replace ? "CREATE OR REPLACE FUNCTION " : "CREATE FUNCTION ";
    append_db_qualified_identifier(sql, name, driver_);
    sql += "() RETURNS trigger LANGUAGE plpgsql AS ";
    detail::append_db_dollar_body(sql, body);
    append(std::move(sql));
}
void db_schema::drop_trigger_function(std::string_view name, db_drop_behavior behavior, bool if_exists) {
    require_postgresql();
    std::string sql;
    sql.reserve(40U + name.size());
    sql += "DROP FUNCTION ";
    if (if_exists) {
        sql += "IF EXISTS ";
    }
    append_db_qualified_identifier(sql, name, driver_);
    sql += "()";
    append_drop_behavior(sql, behavior);
    append(std::move(sql));
}
void db_schema::create_function(const db_function_definition& function) {
    require_postgresql();
    if (function.name_.empty() || function.body_.empty() ||
        function.body_.view().find('\0') != std::string_view::npos) {
        throw std::invalid_argument("function name and non-NUL body are required");
    }
    std::string sql;
    sql.reserve((function.replace_ ? 27U : 16U) + function.name_.size() + function.parameters_.size() * 24U + 48U + function.body_.size() + 16U);
    sql += function.replace_ ? "CREATE OR REPLACE FUNCTION " : "CREATE FUNCTION ";
    append_db_qualified_identifier(sql, function.name_, driver_);
    sql += '(';
    for (std::size_t i = 0; i < function.parameters_.size(); ++i) {
        if (i != 0) {
            sql += ", ";
        }
        const auto& param = function.parameters_[i];
        if (!param.name_.empty()) {
            append_db_identifier(sql, param.name_, driver_);
            sql += ' ';
        }
        append_type(sql, param.type_);
        if (!param.default_value_.empty()) {
            sql += " DEFAULT ";
            append_expression(sql, param.default_value_);
        }
    }
    sql += ") RETURNS ";
    if (function.returns_set_) {
        sql += "SETOF ";
    }
    append_type(sql, function.return_type_);
    sql += " LANGUAGE ";
    sql += function_language_name(function.language_);
    sql += function_volatility_clause(function.volatility_);
    if (function.strict_) {
        sql += " STRICT";
    }
    sql += " AS ";
    detail::append_db_dollar_body(sql, function.body_);
    append(std::move(sql));
}
void db_schema::create_function(const db_function_definition& function, const db_procedure& body) {
    const auto proc_body = body.body();
    auto copy = function;
    copy.language_ = db_function_language::plpgsql;
    copy.body_ = proc_body;
    create_function(copy);
}
void db_schema::create_function(const db_function_definition& function, const db_query& sql_body) {
    const auto compiled = sql_body.compile(driver_, db_parameter_mode::literal);
    auto copy = function;
    copy.language_ = db_function_language::sql;
    copy.body_ = compiled.sql();
    create_function(copy);
}
void db_schema::drop_function(std::string_view name, std::span<const db_type_definition> parameter_types,
    db_drop_behavior behavior, bool if_exists) {
    require_postgresql();
    std::string sql;
    sql.reserve(28U + name.size() + parameter_types.size() * 12U + (behavior == db_drop_behavior::cascade ? 8U : 0U));
    sql += "DROP FUNCTION ";
    if (if_exists) {
        sql += "IF EXISTS ";
    }
    append_db_qualified_identifier(sql, name, driver_);
    sql += '(';
    for (std::size_t i = 0; i < parameter_types.size(); ++i) {
        if (i != 0) {
            sql += ", ";
        }
        append_type(sql, parameter_types[i]);
    }
    sql += ')';
    append_drop_behavior(sql, behavior);
    append(std::move(sql));
}
void db_schema::lock_table(std::string_view table_value, db_table_lock_mode mode, bool nowait) {
    require_postgresql();
    std::string sql;
    sql.reserve(48U + table_value.size());
    detail::append_db_lock_table_statement(sql, table_value, mode, nowait);
    append(std::move(sql));
}
void db_schema::create_trigger(const db_trigger_definition& trigger) {
    require_postgresql();
    if (trigger.events_.empty()) {
        throw std::invalid_argument("trigger requires an event");
    }
    bool update = false;
    bool truncate = false;
    for (std::size_t i = 0; i < trigger.events_.size(); ++i) {
        update |= trigger.events_[i] == db_trigger_event::update;
        truncate |= trigger.events_[i] == db_trigger_event::truncate;
        for (std::size_t j = 0; j < i; ++j) {
            if (trigger.events_[i] == trigger.events_[j]) {
                throw std::invalid_argument("duplicate trigger event");
            }
        }
    }
    if (!update && !trigger.update_columns_.empty()) {
        throw std::invalid_argument("trigger UPDATE OF requires an update event");
    }
    if (truncate && trigger.for_each_row_) {
        throw std::invalid_argument("TRUNCATE triggers run for each statement");
    }
    if (trigger.timing_ == db_trigger_timing::instead_of && (!trigger.for_each_row_ || !trigger.when_.empty() || !trigger.update_columns_.empty())) {
        throw std::invalid_argument("INSTEAD OF requires a row trigger without WHEN or UPDATE OF");
    }
    if (!trigger.old_table_.empty() || !trigger.new_table_.empty()) {
        if (trigger.timing_ != db_trigger_timing::after || trigger.events_.size() != 1 || !trigger.update_columns_.empty()) {
            throw std::invalid_argument("trigger transition tables require an AFTER trigger for one event without UPDATE OF");
        }
        const auto event = trigger.events_.front();
        if (!trigger.old_table_.empty() && event != db_trigger_event::update && event != db_trigger_event::delete_value) {
            throw std::invalid_argument("trigger OLD TABLE requires an UPDATE or DELETE event");
        }
        if (!trigger.new_table_.empty() && event != db_trigger_event::insert && event != db_trigger_event::update) {
            throw std::invalid_argument("trigger NEW TABLE requires an INSERT or UPDATE event");
        }
        if (!trigger.old_table_.empty() && trigger.old_table_ == trigger.new_table_) {
            throw std::invalid_argument("trigger OLD TABLE and NEW TABLE must have distinct names");
        }
    }
    std::string sql;
    sql.reserve(96U + trigger.name_.size() + trigger.table_.size() + trigger.function_.size() +
                trigger.update_columns_.size() * 16U + trigger.arguments_.size() * 16U);
    sql += "CREATE TRIGGER ";
    append_db_identifier(sql, trigger.name_, driver_);
    sql += trigger_timing_clause(trigger.timing_);
    bool first = true;
    for (const auto event : trigger.events_) {
        if (!first) {
            sql += " OR ";
        }
        first = false;
        sql += trigger_event_name(event);
        if (event == db_trigger_event::update && !trigger.update_columns_.empty()) {
            sql += " OF ";
            for (std::size_t i = 0; i < trigger.update_columns_.size(); ++i) {
                if (i != 0) {
                    sql += ", ";
                }
                append_db_identifier(sql, trigger.update_columns_[i], driver_);
            }
        }
    }
    sql += " ON ";
    append_db_qualified_identifier(sql, trigger.table_, driver_);
    if (!trigger.old_table_.empty() || !trigger.new_table_.empty()) {
        sql += " REFERENCING";
        if (!trigger.old_table_.empty()) {
            sql += " OLD TABLE AS ";
            append_db_identifier(sql, trigger.old_table_, driver_);
        }
        if (!trigger.new_table_.empty()) {
            sql += " NEW TABLE AS ";
            append_db_identifier(sql, trigger.new_table_, driver_);
        }
    }
    sql += trigger.for_each_row_ ? " FOR EACH ROW" : " FOR EACH STATEMENT";
    if (!trigger.when_.empty()) {
        sql += " WHEN (";
        append_expression(sql, trigger.when_);
        sql += ')';
    }
    sql += " EXECUTE FUNCTION ";
    append_db_qualified_identifier(sql, trigger.function_, driver_);
    sql += '(';
    for (std::size_t i = 0; i < trigger.arguments_.size(); ++i) {
        if (i != 0) {
            sql += ", ";
        }
        append_db_string_literal(sql, trigger.arguments_[i], driver_);
    }
    sql += ')';
    append(std::move(sql));
}
void db_schema::drop_trigger(std::string_view table_value, std::string_view name, bool if_exists) {
    require_postgresql();
    std::string sql;
    sql.reserve(40U + name.size() + table_value.size());
    sql += "DROP TRIGGER ";
    if (if_exists) {
        sql += "IF EXISTS ";
    }
    append_db_identifier(sql, name, driver_);
    sql += " ON ";
    append_db_qualified_identifier(sql, table_value, driver_);
    append(std::move(sql));
}
void db_schema::set_table_options(std::string_view table_value, std::span<const db_table_option> options) {
    require_postgresql();
    if (options.empty()) {
        throw std::invalid_argument("table options cannot be empty");
    }
    for (std::size_t i = 0; i < options.size(); ++i) {
        for (std::size_t j = 0; j < i; ++j) {
            if (options[i].name_ == options[j].name_) {
                throw std::invalid_argument("duplicate table option");
            }
        }
    }
    auto sql = begin_alter_table(driver_, table_value, 32U + options.size() * 48U);
    sql += " SET (";
    for (std::size_t i = 0; i < options.size(); ++i) {
        if (i != 0) {
            sql += ", ";
        }
        append_db_qualified_identifier(sql, options[i].name_, driver_);
        sql += " = ";
        std::visit([&](const auto& value) {
            using t_type = std::remove_cvref_t<decltype(value)>;
            if constexpr (std::is_same_v<t_type, bool>) {
                sql += value ? "true" : "false";
            } else if constexpr (std::is_same_v<t_type, std::int64_t>) {
                detail::append_db_number(sql, value);
            } else {
                append_db_string_literal(sql, value, driver_);
            }
        },
            options[i].value_);
    }
    sql += ')';
    append(std::move(sql));
}
void db_schema::set_database_option(std::string_view database_value, std::string_view name, std::string_view value) {
    require_postgresql();
    std::string sql;
    sql.reserve(32U + database_value.size() + name.size() + value.size());
    sql += "ALTER DATABASE ";
    append_db_identifier(sql, database_value, driver_);
    sql += " SET ";
    append_db_qualified_identifier(sql, name, driver_);
    sql += " TO ";
    append_db_string_literal(sql, value, driver_);
    append(std::move(sql));
}
void db_schema::create_hypertable(const db_hypertable_options& options) {
    require_postgresql();
    // Validate names separately from the SQL text values consumed by Timescale.
    auto sql = begin_timescale_perform(
        driver_, "create_hypertable", options.table_, 160U + options.time_column_.size());
    detail::validate_db_identifier(options.time_column_, driver_);
    sql += ", \"by_range\"(";
    append_db_string_literal(sql, options.time_column_, driver_);
    if (!options.chunk_interval_.empty()) {
        sql += ", ";
        append_expression(sql, options.chunk_interval_);
    }
    sql += "), \"create_default_indexes\" => ";
    sql += options.create_default_indexes_ ? "TRUE" : "FALSE";
    sql += ", \"if_not_exists\" => ";
    sql += options.if_not_exists_ ? "TRUE" : "FALSE";
    sql += ", \"migrate_data\" => ";
    sql += options.migrate_data_ ? "TRUE" : "FALSE";
    sql += ')';
    append(std::move(sql), db_migration_atomicity::transactional, true);
}
void db_schema::set_chunk_time_interval(std::string_view table_value, db_expression interval) {
    require_postgresql();
    auto sql = begin_timescale_perform(driver_, "set_chunk_time_interval", table_value, 64U);
    sql += ", ";
    append_expression(sql, interval);
    sql += ')';
    append(std::move(sql), db_migration_atomicity::transactional, true);
}
void db_schema::set_compression(std::string_view table_value, const db_compression_options& options) {
    require_postgresql();
    if (!options.enabled_ && (!options.segment_by_.empty() || !options.order_by_.empty())) {
        throw std::invalid_argument("disabled compression cannot specify segment or order columns");
    }
    std::array<db_table_option, 3> values{db_table_option{"timescaledb.compress", options.enabled_}, {}, {}};
    std::size_t value_count = 1U;
    std::string segments;
    if (!options.segment_by_.empty()) {
        segments.reserve(options.segment_by_.size() * 16U);
        for (const auto& column : options.segment_by_) {
            if (!segments.empty()) {
                segments += ", ";
            }
            append_db_identifier(segments, column, driver_);
        }
        values[value_count++] = {"timescaledb.compress_segmentby", segments};
    }
    std::string order;
    if (!options.order_by_.empty()) {
        order.reserve(options.order_by_.size() * 24U);
        for (const auto& column : options.order_by_) {
            if (!order.empty()) {
                order += ", ";
            }
            append_db_identifier(order, column.column_, driver_);
            order += column.direction_ == db_order_direction::asc ? " ASC" : " DESC";
            if (column.nulls_ == db_nulls_order::first) {
                order += " NULLS FIRST";
            }
            if (column.nulls_ == db_nulls_order::last) {
                order += " NULLS LAST";
            }
        }
        values[value_count++] = {"timescaledb.compress_orderby", order};
    }
    set_table_options(table_value, std::span<const db_table_option>(values.data(), value_count));
}
void db_schema::add_compression_policy(std::string_view table_value, db_expression after, bool if_not_exists) {
    require_postgresql();
    append(make_add_timescale_policy_sql(
               driver_, "add_compression_policy", table_value, "compress_after", after, if_not_exists),
        db_migration_atomicity::transactional, true);
}
void db_schema::remove_compression_policy(std::string_view table_value, bool if_exists) {
    require_postgresql();
    append(make_remove_timescale_policy_sql(driver_, "remove_compression_policy", table_value, if_exists),
        db_migration_atomicity::transactional, true);
}
void db_schema::add_retention_policy(std::string_view table_value, db_expression after, bool if_not_exists) {
    require_postgresql();
    append(make_add_timescale_policy_sql(
               driver_, "add_retention_policy", table_value, "drop_after", after, if_not_exists),
        db_migration_atomicity::transactional, true);
}
void db_schema::remove_retention_policy(std::string_view table_value, bool if_exists) {
    require_postgresql();
    append(make_remove_timescale_policy_sql(driver_, "remove_retention_policy", table_value, if_exists),
        db_migration_atomicity::transactional, true);
}
std::vector<db_migration> db_schema::compile(std::string_view id) const {
    std::vector<db_migration> out;
    if (id.empty()) {
        throw std::invalid_argument("migration id is required");
    }
    if (driver_ == db_driver::postgresql) {
        bool unwrapped = false;
        for (auto& s : statements_) {
            if (s.atomicity_ == db_migration_atomicity::unwrapped) {
                unwrapped = true;
            }
        }
        if (unwrapped && statements_.size() != 1) {
            throw std::invalid_argument("unwrapped schema operation must be alone");
        }
        if (statements_.size() == 1 && !statements_[0].procedural_) {
            out.reserve(1);
            out.emplace_back(db_migration_options{
                std::string(id), std::string(statements_[0].sql_), statements_[0].atomicity_});
        } else if (!statements_.empty()) {
            out.reserve(1);
            std::size_t body_size = 12U;
            for (const auto& s : statements_) {
                body_size += s.sql_.size() + 1U;
            }
            std::array<char, 32> delimiter_buf;
            const auto delimiter = detail::select_db_dollar_delimiter(
                delimiter_buf, [&](std::string_view candidate) noexcept {
                    return std::ranges::any_of(statements_, [&](const auto& s) {
                        return s.sql_.find(candidate) != std::string_view::npos;
                    });
                });
            std::string sql;
            sql.reserve(3U + delimiter.size() * 2U + body_size);
            sql += "DO ";
            sql.append(delimiter);
            sql.append(" BEGIN ");
            for (const auto& s : statements_) {
                sql.append(s.sql_);
                sql.push_back(';');
            }
            sql.append(" END ");
            sql.append(delimiter);
            out.emplace_back(db_migration_options{
                std::string(id), std::move(sql), db_migration_atomicity::transactional});
        }
    } else {
        out.reserve(statements_.size());
        for (size_t i = 0; i < statements_.size(); ++i) {
            std::array<char, http_max_unsigned_decimal_size> digits;
            const auto suffix = http_format_unsigned(digits, static_cast<std::uint64_t>(i + 1U));
            std::string numbered;
            numbered.reserve(id.size() + 1U + suffix.size());
            numbered.append(id);
            numbered.push_back('_');
            numbered.append(suffix);
            out.emplace_back(db_migration_options{
                std::move(numbered), std::string(statements_[i].sql_), statements_[i].atomicity_});
        }
    }
    return out;
}

}  // namespace ruvia

namespace ruvia {
namespace {
constexpr auto pg = db_driver::postgresql;
void require_sql_state(std::string_view state_value) {
    if (state_value.size() != 5 || state_value == "00000" || !std::ranges::all_of(state_value, [](char ch) { return (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z'); })) {
        throw std::invalid_argument("SQLSTATE must have five uppercase alphanumeric characters and indicate failure");
    }
}
class procedure_append_guard final {
public:
    explicit procedure_append_guard(std::string& target, std::size_t extra_reserve = 48U)
        : target_(target),
          initial_size_(target.size()) {
        if (target_.capacity() - initial_size_ < extra_reserve) {
            target_.reserve(initial_size_ + (std::max)(extra_reserve, std::size_t{64}));
        }
    }

    procedure_append_guard(const procedure_append_guard&) = delete;
    procedure_append_guard& operator=(const procedure_append_guard&) = delete;

    void commit() noexcept {
        committed_ = true;
    }

    ~procedure_append_guard() {
        if (!committed_) {
            target_.resize(initial_size_);
        }
    }

private:
    std::string& target_;
    std::size_t initial_size_;
    bool committed_{false};
};
}  // namespace

db_procedure::db_procedure() noexcept = default;
void db_procedure::append_expression(std::string& output, db_expression value) const {
    detail::append_db_expression(output, value, pg);
}
void db_procedure::require_unique_variable(std::string_view name) const {
    if (std::ranges::any_of(variables_, [&](const auto& existing) { return existing == name; })) {
        throw std::invalid_argument("duplicate procedure variable");
    }
}
void db_procedure::declare_variable(const db_variable_definition& variable) {
    require_unique_variable(variable.name_);
    procedure_append_guard guard(declarations_, 32U + variable.name_.size());
    detail::append_db_identifier(declarations_, variable.name_, pg);
    declarations_ += ' ';
    const auto& t = variable.type_;
    detail::append_db_type_name(
        declarations_, t.data_type_, t.custom_name_, t.length_, t.precision_, t.scale_, t.array_, pg);
    if (!variable.default_value_.empty()) {
        declarations_ += " := ";
        append_expression(declarations_, variable.default_value_);
    }
    declarations_ += ";\n";
    variables_.emplace_back(variable.name_);
    guard.commit();
}
void db_procedure::declare_row(std::string_view name, std::string_view table_value) {
    require_unique_variable(name);
    procedure_append_guard guard(declarations_, 16U + name.size() + table_value.size());
    detail::append_db_identifier(declarations_, name, pg);
    declarations_ += ' ';
    detail::append_db_qualified_identifier(declarations_, table_value, pg);
    declarations_ += "%ROWTYPE;\n";
    variables_.emplace_back(name);
    guard.commit();
}
void db_procedure::declare_record(std::string_view name) {
    require_unique_variable(name);
    procedure_append_guard guard(declarations_, name.size() + 11U);
    detail::append_db_identifier(declarations_, name, pg);
    declarations_ += " RECORD;\n";
    variables_.emplace_back(name);
    guard.commit();
}
void db_procedure::assign(std::string_view target, db_expression value) {
    procedure_append_guard guard(statements_, 32U + target.size());
    detail::append_db_qualified_identifier(statements_, target, pg);
    statements_ += " := ";
    append_expression(statements_, value);
    statements_ += ";\n";
    guard.commit();
}
void db_procedure::select_into(const db_query& query, std::span<const std::string_view> targets, bool strict) {
    if (targets.empty() || !query.returns_rows()) {
        throw std::invalid_argument("SELECT INTO requires returned rows and target variables");
    }
    const auto statement = query.compile(pg, db_parameter_mode::literal);
    procedure_append_guard guard(statements_, statement.sql().size() + 16U + targets.size() * 16U);
    statements_ += statement.sql();
    statements_ += strict ? " INTO STRICT " : " INTO ";
    bool first = true;
    for (const auto target : targets) {
        if (!first) {
            statements_ += ", ";
        }
        first = false;
        detail::append_db_qualified_identifier(statements_, target, pg);
    }
    statements_ += ";\n";
    guard.commit();
}
void db_procedure::execute(const db_query& query) {
    if (query.returns_rows()) {
        throw std::invalid_argument("procedure execute requires a command; consume returned rows with select_into");
    }
    const auto statement = query.compile(pg, db_parameter_mode::literal);
    procedure_append_guard guard(statements_, statement.sql().size() + 2U);
    statements_ += statement.sql();
    statements_ += ";\n";
    guard.commit();
}
void db_procedure::perform(db_expression expression) {
    procedure_append_guard guard(statements_, 48U);
    detail::append_db_perform_statement(statements_, expression);
    statements_ += ";\n";
    guard.commit();
}
void db_procedure::apply(const db_schema& schema) {
    if (schema.driver_ != pg) {
        throw std::invalid_argument("PL/pgSQL requires a PostgreSQL schema");
    }
    for (const auto& statement : schema.statements_) {
        if (statement.atomicity_ != db_migration_atomicity::transactional) {
            throw std::invalid_argument("nontransactional schema operations cannot run in a procedure");
        }
    }
    procedure_append_guard guard(statements_, 0);
    for (const auto& statement : schema.statements_) {
        statements_ += statement.sql_;
        statements_ += ";\n";
    }
    guard.commit();
}
void db_procedure::begin_if(db_expression condition) {
    procedure_append_guard guard(statements_, 48U);
    statements_ += "IF ";
    append_expression(statements_, condition);
    statements_ += " THEN\n";
    blocks_.push_back(block_type::if_value);
    guard.commit();
}
void db_procedure::else_if(db_expression condition) {
    if (blocks_.empty() || blocks_.back() != block_type::if_value) {
        throw std::invalid_argument("ELSEIF requires an open IF without ELSE");
    }
    procedure_append_guard guard(statements_, 48U);
    statements_ += "ELSIF ";
    append_expression(statements_, condition);
    statements_ += " THEN\n";
    guard.commit();
}
void db_procedure::otherwise() {
    if (blocks_.empty() || blocks_.back() != block_type::if_value) {
        throw std::invalid_argument("ELSE requires an open IF without ELSE");
    }
    statements_ += "ELSE\n";
    blocks_.back() = block_type::else_value;
}
void db_procedure::end_if() {
    if (blocks_.empty() || (blocks_.back() != block_type::if_value && blocks_.back() != block_type::else_value)) {
        throw std::invalid_argument("END IF requires an open IF");
    }
    statements_ += "END IF;\n";
    blocks_.pop_back();
}
void db_procedure::begin_block() {
    procedure_append_guard guard(statements_, 6U);
    statements_ += "BEGIN\n";
    blocks_.push_back(block_type::begin);
    guard.commit();
}
void db_procedure::catch_sql_state(std::string_view state_value) {
    if (blocks_.empty() || (blocks_.back() != block_type::begin && blocks_.back() != block_type::exception)) {
        throw std::invalid_argument("exception handler requires an open block");
    }
    require_sql_state(state_value);
    const std::string_view prefix = blocks_.back() == block_type::begin ? "EXCEPTION\nWHEN SQLSTATE "
                                                                        : "WHEN SQLSTATE ";
    procedure_append_guard guard(statements_, prefix.size() + 20U);
    statements_.append(prefix);
    detail::append_db_string_literal(statements_, state_value, pg);
    statements_ += " THEN\nNULL;\n";
    blocks_.back() = block_type::exception;
    guard.commit();
}
void db_procedure::end_block() {
    if (blocks_.empty() || (blocks_.back() != block_type::begin && blocks_.back() != block_type::exception)) {
        throw std::invalid_argument("END requires an open block");
    }
    statements_ += "END;\n";
    blocks_.pop_back();
}
void db_procedure::begin_for_query(std::string_view target, const db_query& query) {
    if (!query.returns_rows()) {
        throw std::invalid_argument("FOR IN query loop requires a row-returning query");
    }
    const auto statement = query.compile(pg, db_parameter_mode::literal);
    procedure_append_guard guard(statements_, 16U + target.size() + statement.sql().size());
    statements_.append("FOR ");
    detail::append_db_identifier(statements_, target, pg);
    statements_.append(" IN ");
    statements_.append(statement.sql());
    statements_.append(" LOOP\n");
    blocks_.push_back(block_type::loop);
    guard.commit();
}
void db_procedure::begin_foreach(std::string_view target, db_expression array, unsigned slice) {
    procedure_append_guard guard(statements_, 48U + target.size());
    statements_ += "FOREACH ";
    detail::append_db_identifier(statements_, target, pg);
    if (slice > 0) {
        statements_ += " SLICE ";
        detail::append_db_number(statements_, static_cast<std::uint64_t>(slice));
    }
    statements_ += " IN ARRAY ";
    append_expression(statements_, array);
    statements_ += " LOOP\n";
    blocks_.push_back(block_type::loop);
    guard.commit();
}
void db_procedure::end_loop() {
    if (blocks_.empty() || blocks_.back() != block_type::loop) {
        throw std::invalid_argument("END LOOP requires an open FOR or FOREACH loop");
    }
    statements_ += "END LOOP;\n";
    blocks_.pop_back();
}
void db_procedure::execute_dynamic(db_expression command, std::span<const db_expression> using_args) {
    procedure_append_guard guard(statements_, 48U + using_args.size() * 24U);
    statements_ += "EXECUTE ";
    append_expression(statements_, command);
    if (!using_args.empty()) {
        statements_ += " USING ";
        for (std::size_t i = 0; i < using_args.size(); ++i) {
            if (i != 0) {
                statements_ += ", ";
            }
            append_expression(statements_, using_args[i]);
        }
    }
    statements_ += ";\n";
    guard.commit();
}
void db_procedure::lock_table(std::string_view table, db_table_lock_mode mode, bool nowait) {
    procedure_append_guard guard(statements_, 48U + table.size());
    detail::append_db_lock_table_statement(statements_, table, mode, nowait);
    statements_.append(";\n");
    guard.commit();
}
void db_procedure::return_value(db_expression expression) {
    procedure_append_guard guard(statements_, 32U);
    statements_ += "RETURN";
    if (!expression.empty()) {
        statements_ += ' ';
        append_expression(statements_, expression);
    }
    statements_ += ";\n";
    guard.commit();
}
void db_procedure::raise_exception(std::string_view message, std::string_view sql_state) {
    require_sql_state(sql_state);
    procedure_append_guard guard(statements_, 56U + message.size() + sql_state.size());
    statements_ += "RAISE EXCEPTION USING MESSAGE = ";
    detail::append_db_string_literal(statements_, message, pg);
    statements_ += ", ERRCODE = ";
    detail::append_db_string_literal(statements_, sql_state, pg);
    statements_ += ";\n";
    guard.commit();
}
std::string db_procedure::body() const {
    if (!blocks_.empty()) {
        throw std::invalid_argument("procedure has unclosed control-flow blocks");
    }
    const std::string_view statements =
        statements_.empty() ? std::string_view("NULL;\n") : std::string_view(statements_);
    std::string result;
    result.reserve((!declarations_.empty() ? 8U + declarations_.size() : 0U) + 6U + statements.size() + 3U);
    if (!declarations_.empty()) {
        result.append("DECLARE\n");
        result.append(declarations_);
    }
    result.append("BEGIN\n");
    result.append(statements);
    result.append("END");
    return result;
}

}  // namespace ruvia

namespace ruvia {
db_value::db_value(std::nullptr_t)
    : storage_(std::monostate{}) {}

db_value::db_value(const char* value)
    : storage_(value == nullptr ? storage_type(std::monostate{})
                                : storage_type(std::in_place_type<borrowed_text>, value)) {}

db_value::db_value(std::string_view value)
    : storage_(std::in_place_type<borrowed_text>, value) {}

db_value::db_value(std::in_place_type_t<std::string>, std::string value)
    : storage_(std::in_place_type<std::string>, std::move(value)) {}

db_value::db_value(const db_value& other)
    : storage_(std::visit(
          [](const auto& value) -> storage_type {
              using value_type = std::remove_cvref_t<decltype(value)>;
              if constexpr (std::is_same_v<value_type, std::string>) {
                  return storage_type(std::in_place_type<value_type>, value, value.get_allocator());
              } else {
                  return storage_type(std::in_place_type<value_type>, value);
              }
          },
          other.storage_)) {}

db_value::db_value(bool value)
    : storage_(std::in_place_type<bool>, value) {}

db_value_type db_value::type() const noexcept {
    return std::visit(
        [](const auto& value) noexcept {
            using value_type = std::remove_cvref_t<decltype(value)>;
            if constexpr (std::is_same_v<value_type, std::monostate>) {
                return db_value_type::null;
            } else if constexpr (std::is_same_v<value_type, borrowed_text> ||
                                 std::is_same_v<value_type, std::string>) {
                return db_value_type::string;
            } else if constexpr (std::is_same_v<value_type, std::int64_t>) {
                return db_value_type::signed_value;
            } else if constexpr (std::is_same_v<value_type, std::uint64_t>) {
                return db_value_type::unsigned_value;
            } else if constexpr (std::is_same_v<value_type, double>) {
                return db_value_type::double_value;
            } else {
                return db_value_type::bool_value;
            }
        },
        storage_);
}

std::string_view db_value::text() const& {
    if (const auto* borrowed = std::get_if<borrowed_text>(&storage_)) {
        return borrowed->view();
    }
    return std::get<std::string>(storage_);
}

std::int64_t db_value::signed_value() const {
    return std::get<std::int64_t>(storage_);
}

std::uint64_t db_value::unsigned_value() const {
    return std::get<std::uint64_t>(storage_);
}

double db_value::double_value() const {
    return std::get<double>(storage_);
}

bool db_value::bool_value() const {
    return std::get<bool>(storage_);
}

db_field::db_field() noexcept = default;

db_field::db_field(std::nullptr_t) noexcept {}

db_field::db_field(std::string_view value)
    : storage_(std::in_place_type<std::string>, value) {}

db_field::db_field(borrowed_tag_type, std::string_view value) noexcept
    : storage_(std::in_place_type<borrowed_text>, value) {}

db_field db_field::borrowed(std::string_view value) noexcept {
    return db_field(borrowed_tag_type{}, value);
}

db_field::db_field(db_field&& other) noexcept
    : storage_(std::move(other.storage_)) {
    other.storage_.emplace<std::monostate>();
}

// NOLINTNEXTLINE(performance-noexcept-move-constructor)
db_field& db_field::operator=(db_field&& other) noexcept = default;

std::optional<std::string_view> db_field::value() const& noexcept {
    if (const auto* owned = std::get_if<std::string>(&storage_)) {
        return *owned;
    }
    if (const auto* borrowed = std::get_if<borrowed_text>(&storage_)) {
        return borrowed->view();
    }
    return std::nullopt;
}

db_row::db_row(const db_field* fields_value, std::size_t size, const std::string* column_names,
    std::size_t column_count) noexcept
    : fields_(fields_value, size),
      column_names_(column_names, column_count) {}

db_row::db_row(db_row&& other) noexcept
    : fields_(std::exchange(other.fields_, {})),
      column_names_(std::exchange(other.column_names_, {})) {}

db_row& db_row::operator=(db_row&& other) noexcept {
    if (this != &other) {
        fields_ = std::exchange(other.fields_, {});
        column_names_ = std::exchange(other.column_names_, {});
    }
    return *this;
}

bool db_row::empty() const noexcept {
    return fields_.empty();
}

std::size_t db_row::size() const noexcept {
    return fields_.size();
}

const db_field& db_row::operator[](std::size_t index) const& noexcept {
    return fields_[index];
}

bool db_row::contains(std::string_view column) const noexcept {
    return find(column) != nullptr;
}

const db_field* db_row::find(std::string_view column) const& noexcept {
    for (std::size_t index = 0; index < column_names_.size(); ++index) {
        if (column_names_[index] == column) {
            return &fields_[index];
        }
    }
    return nullptr;
}

const db_field& db_row::operator[](std::string_view column) const& {
    if (const auto* field = find(column)) {
        return *field;
    }
    throw std::out_of_range("database result has no such column");
}

const db_field* db_row::begin() const& noexcept {
    return fields_.data();
}

const db_field* db_row::end() const& noexcept {
    const auto* first = begin();
    return fields_.empty() ? first : first + fields_.size();
}

std::span<const std::string> db_row::column_names() const& noexcept {
    return column_names_;
}

db_rows::db_rows() noexcept = default;

db_rows::db_rows(db_rows&& other) noexcept
    : rows_(std::move(other.rows_)),
      fields_(std::move(other.fields_)),
      column_names_(std::move(other.column_names_)),
      raw_result_(other.raw_result_) {
    other.raw_result_.template emplace<no_raw_result_type>();
}

db_rows::~db_rows() {
    if (const auto* owned = std::get_if<owned_raw_result_type>(&raw_result_)) {
        owned->release_(owned->value_);
    }
}

db_rows& db_rows::operator=(db_rows&& other) {
    if (this == &other) {
        return *this;
    }
    if (const auto* owned = std::get_if<owned_raw_result_type>(&raw_result_)) {
        owned->release_(owned->value_);
    }
    rows_ = std::move(other.rows_);
    fields_ = std::move(other.fields_);
    column_names_ = std::move(other.column_names_);
    raw_result_ = other.raw_result_;
    other.raw_result_.template emplace<no_raw_result_type>();
    return *this;
}

bool db_rows::empty() const noexcept {
    return rows_.empty();
}

std::size_t db_rows::size() const noexcept {
    return rows_.size();
}

const db_row& db_rows::operator[](std::size_t index) const& noexcept {
    return rows_[index];
}

const db_row* db_rows::begin() const& noexcept {
    return rows_.data();
}

const db_row* db_rows::end() const& noexcept {
    const auto* first = begin();
    return rows_.empty() ? first : first + rows_.size();
}

const db_row& db_rows::front() const& noexcept {
    return rows_.front();
}

const db_row& db_rows::back() const& noexcept {
    return rows_.back();
}

std::span<const std::string> db_rows::column_names() const& noexcept {
    return column_names_;
}

db_migration_report::db_migration_report() noexcept = default;

db_migration_report& db_migration_report::operator=(db_migration_report&& other) = default;
std::span<const std::string> db_migration_report::applied() const& noexcept {
    return applied_;
}

std::span<const std::string> db_migration_report::skipped() const& noexcept {
    return skipped_;
}

bool db_migration_report::changed() const noexcept {
    return !applied_.empty();
}

}  // namespace ruvia

namespace ruvia::detail {

namespace {
void require_name(std::string_view name, bool qualified = false) {
    if (name.empty() || name.find('\0') != std::string_view::npos) {
        throw std::invalid_argument("database identifier must be nonempty and contain no NUL");
    }
    if (qualified && (name.front() == '.' || name.back() == '.' || name.find("..") != std::string_view::npos)) {
        throw std::invalid_argument("database qualified identifier has an empty component");
    }
}
// FROM/JOIN/target aliases, CTE names and lock targets are emitted as one
// identifier but referenced through dot-splitting qualifiers, so a dotted
// name could never be referenced.
void require_unqualified_name(std::string_view name) {
    require_name(name);
    if (name.find('.') != std::string_view::npos) {
        throw std::invalid_argument("database alias, CTE or lock name must be one unqualified identifier");
    }
}
// Switching a query to INSERT, UPDATE or DELETE keeps only its WHERE and WITH
// clauses: a retained FROM would turn into UPDATE FROM / DELETE USING and
// silently widen the target, and the other clauses mean nothing there.
void require_statement_kind_change(const db_query_storage& s, db_query_kind next) {
    if (s.kind_ == next) {
        return;
    }
    if (s.source_ || !s.joins_.empty() || !s.projections_.empty() || !s.rows_.empty() ||
        !s.assignments_.empty() || s.insert_query_ != no_db_node || s.conflict_ || !s.groups_.empty() ||
        s.having_ != no_db_node || !s.orders_.empty() || s.limit_ || s.offset_ || s.lock_ ||
        !s.set_operations_.empty() || s.distinct_ || !s.distinct_on_.empty()) {
        throw std::invalid_argument("statement kind change requires a query without FROM/JOIN/SELECT/VALUES clauses");
    }
}
void set_source_options(db_query_source& source_value, const db_source_options& options) {
    source_value.lateral_ = options.lateral_;
    source_value.ordinality_ = options.with_ordinality_;
    source_value.columns_.reserve(options.columns_.size());
    for (const auto& column : options.columns_) {
        require_name(column.name_);
        source_value.columns_.emplace_back(column);
    }
}
void validate_db_sql_parts(std::span<const std::string_view> parts, std::size_t argument_count) {
    if (parts.size() != argument_count + 1 || (argument_count == 0 && parts.front().empty())) {
        throw std::invalid_argument("SQL expression requires one more syntax part than arguments");
    }
    for (const auto part : parts) {
        if (part.find('\0') != std::string_view::npos) {
            throw std::invalid_argument("SQL expression cannot contain NUL");
        }
    }
}
void require_variadic_arguments(std::size_t count, std::string_view function) {
    if (count == 0) {
        throw std::invalid_argument(std::string(function) + " requires an argument");
    }
}
// Appending through reserve(size() + extra) would allocate exactly and move
// every element on each later append; keep geometric growth instead.
template <class vector_type>
void reserve_append(vector_type& values, std::size_t extra) {
    if (values.capacity() - values.size() >= extra) {
        return;
    }
    if (extra > values.max_size() - values.size()) {
        throw std::length_error("database query storage exceeds its maximum size");
    }
    const auto doubled = values.capacity() > values.max_size() / 2 ? values.max_size() : values.capacity() * 2;
    values.reserve((std::max)(values.size() + extra, doubled));
}
template <class column_range>
void validate_cte(const db_query_storage& s, std::string_view name, const column_range& columns) {
    require_unqualified_name(name);
    if (std::ranges::any_of(s.ctes_, [&](const auto& existing) { return existing.name_ == name; })) {
        throw std::invalid_argument("duplicate CTE name");
    }
    for (const auto& column : columns) {
        require_name(std::string_view(column));
    }
}
template <class column_range>
db_stored_cte prepare_cte(db_query_storage& s, std::string_view name, bool recursive,
    db_materialization materialization, const column_range& columns) {
    db_stored_cte prepared(name, s.queries_.size(), recursive, materialization, columns);
    reserve_append(s.queries_, 1);
    reserve_append(s.ctes_, 1);
    return prepared;
}
}  // namespace

struct node_index_map final {
    static constexpr std::size_t inline_capacity = 16;
    std::array<std::size_t, inline_capacity> inline_{};
    std::vector<std::size_t> heap_;

    node_index_map(std::size_t count) {
        if (count <= inline_capacity) {
            std::fill_n(inline_.data(), count, no_db_node);
        } else {
            heap_.assign(count, no_db_node);
        }
    }

    [[nodiscard]] std::size_t& operator[](std::size_t index) noexcept {
        return heap_.empty() ? inline_[index] : heap_[index];
    }
    [[nodiscard]] std::size_t operator[](std::size_t index) const noexcept {
        return heap_.empty() ? inline_[index] : heap_[index];
    }
};

struct node_visit_set final {
    static constexpr std::size_t inline_capacity = 64;
    std::uint64_t inline_{0};
    std::vector<bool> heap_;

    node_visit_set(std::size_t count) {
        if (count > inline_capacity) {
            heap_.assign(count, false);
        }
    }

    [[nodiscard]] bool mark(std::size_t index) noexcept {
        if (heap_.empty()) {
            const std::uint64_t bit = std::uint64_t{1} << index;
            if ((inline_ & bit) != 0) {
                return false;
            }
            inline_ |= bit;
            return true;
        }
        if (heap_[index]) {
            return false;
        }
        heap_[index] = true;
        return true;
    }
};

// Numbers the nodes and nested queries the current statement reaches, in
// storage order. A builder retains replaced clauses, retired sources and
// abandoned nodes; a copy carries only this graph. The walk is iterative, so
// it accepts any depth the builder accepted.
class reachable_query_graph final {
public:
    reachable_query_graph(const db_query_storage& s)
        : nodes_(s.nodes_.size()),
          queries_(s.queries_.size()) {
        mark_nodes(s.projections_);
        mark_nodes(s.distinct_on_);
        mark_nodes(s.groups_);
        mark_nodes(s.returning_);
        for (const auto& order : s.orders_) {
            mark_node(order.expression_);
        }
        for (const auto& row : s.rows_) {
            mark_nodes(row);
        }
        for (const auto& assignment : s.assignments_) {
            mark_node(assignment.expression_);
        }
        mark_node(s.predicate_);
        mark_node(s.having_);
        mark_query(s.insert_query_);
        if (s.source_) {
            mark_source(*s.source_);
        }
        for (const auto& join : s.joins_) {
            mark_source(join.source_);
            mark_node(join.on_);
        }
        for (const auto& cte : s.ctes_) {
            mark_query(cte.query_);
        }
        for (const auto& operation : s.set_operations_) {
            mark_query(operation.query_);
        }
        if (s.conflict_) {
            mark_node(s.conflict_->target_where_);
            mark_node(s.conflict_->update_where_);
            for (const auto& assignment : s.conflict_->assignments_) {
                mark_node(assignment.expression_);
            }
        }
        while (!pending_.empty()) {
            const auto& current = s.nodes_[pending_.back()];
            pending_.pop_back();
            mark_node(current.left_);
            mark_node(current.right_);
            mark_nodes(current.args_);
            for (const auto& argument : current.named_) {
                mark_node(argument.expression_);
            }
            for (const auto& order : current.orders_) {
                mark_node(order.expression_);
            }
            mark_query(current.query_);
        }
        node_count_ = number(nodes_, s.nodes_.size());
        query_count_ = number(queries_, s.queries_.size());
    }

    // The copy's index for a source index, or no_db_node if unreachable.
    [[nodiscard]] std::size_t node(std::size_t index) const noexcept {
        return index == no_db_node ? no_db_node : nodes_[index];
    }
    [[nodiscard]] std::size_t query(std::size_t index) const noexcept {
        return index == no_db_node ? no_db_node : queries_[index];
    }
    [[nodiscard]] std::size_t node_count() const noexcept {
        return node_count_;
    }
    [[nodiscard]] std::size_t query_count() const noexcept {
        return query_count_;
    }

private:
    static constexpr std::size_t reached = 0;

    void mark_node(std::size_t index) {
        if (index != no_db_node && nodes_[index] == no_db_node) {
            nodes_[index] = reached;
            pending_.push_back(index);
        }
    }
    template <class range_type>
    void mark_nodes(const range_type& indices) {
        for (const auto index : indices) {
            mark_node(index);
        }
    }
    void mark_query(std::size_t index) noexcept {
        if (index != no_db_node) {
            queries_[index] = reached;
        }
    }
    void mark_source(const db_query_source& source_value) {
        mark_node(source_value.expression_);
        mark_query(source_value.query_);
    }
    [[nodiscard]] static std::size_t number(node_index_map& map, std::size_t count) noexcept {
        std::size_t next = 0;
        for (std::size_t index = 0; index < count; ++index) {
            if (map[index] != no_db_node) {
                map[index] = next++;
            }
        }
        return next;
    }

    node_index_map nodes_;
    node_index_map queries_;
    std::vector<std::size_t> pending_;
    std::size_t node_count_{0};
    std::size_t query_count_{0};
};

struct qualified_expression_importer final {
    const std::vector<db_query_node>& source_nodes_;
    const std::vector<db_query>& source_queries_;
    std::vector<db_query_node>& target_nodes_;
    std::vector<db_query>& target_queries_;
    std::string_view source_qualifier_{};
    std::string_view target_qualifier_{};
    bool mark_aliased_{false};
    node_index_map mapped_;

    qualified_expression_importer(const std::vector<db_query_node>& source_nodes,
        const std::vector<db_query>& source_queries, db_query_storage& target,
        std::string_view source_qualifier, std::string_view target_qualifier)
        : source_nodes_(source_nodes),
          source_queries_(source_queries),
          target_nodes_(target.nodes_),
          target_queries_(target.queries_),
          source_qualifier_(source_qualifier),
          target_qualifier_(target_qualifier),
          mapped_(source_nodes.size()) {}

    qualified_expression_importer(const db_query_storage& source_value, db_query_storage& target,
        std::string_view source_qualifier, std::string_view target_qualifier)
        : qualified_expression_importer(
              source_value.nodes_, source_value.queries_, target, source_qualifier, target_qualifier) {}

    qualified_expression_importer(const db_query_storage& source_value,
        std::vector<db_query_node>& target_nodes, std::vector<db_query>& target_queries, bool mark_aliased)
        : source_nodes_(source_value.nodes_),
          source_queries_(source_value.queries_),
          target_nodes_(target_nodes),
          target_queries_(target_queries),
          mark_aliased_(mark_aliased),
          mapped_(source_value.nodes_.size()) {}
    static void require_depth(std::size_t depth) {
        if (depth > max_db_query_depth) {
            throw std::length_error("database expression nesting exceeds 256 levels");
        }
    }

    // Collects the not yet imported left spine of an AND/OR chain, root first.
    static void collect_logical_spine(const std::vector<db_query_node>& nodes, std::size_t id,
        node_index_map& mapped, std::vector<std::size_t>& spine) {
        spine.push_back(id);
        for (auto left = nodes[id].left_;
            continues_logical_chain(nodes, nodes[spine.back()], left) && mapped[left] == no_db_node;
            left = nodes[left].left_) {
            spine.push_back(left);
        }
    }

    void rewrite_subquery_node(db_query_storage& query_storage, node_visit_set& visited,
        bool source_bound, bool target_bound, std::size_t node, std::size_t node_depth) {
        // The left operand is visited last, iteratively: continuing an AND/OR
        // chain does not add a level.
        while (node != no_db_node) {
            require_depth(node_depth);
            if (!visited.mark(node)) {
                return;
            }
            auto& sub_node = query_storage.nodes_[node];
            if (!source_bound && !sub_node.explicit_alias_ &&
                (sub_node.kind_ == db_node_kind::column || sub_node.kind_ == db_node_kind::star) &&
                sub_node.qualifier_ == source_qualifier_) {
                if (target_bound) {
                    throw std::invalid_argument("expression qualifier mapping would capture a correlated reference");
                }
                sub_node.qualifier_ = target_qualifier_;
            }
            rewrite_subquery_node(query_storage, visited, source_bound, target_bound, sub_node.right_, node_depth + 1);
            for (const auto arg : sub_node.args_) {
                rewrite_subquery_node(query_storage, visited, source_bound, target_bound, arg, node_depth + 1);
            }
            for (const auto& arg : sub_node.named_) {
                rewrite_subquery_node(
                    query_storage, visited, source_bound, target_bound, arg.expression_, node_depth + 1);
            }
            for (const auto& order : sub_node.orders_) {
                rewrite_subquery_node(
                    query_storage, visited, source_bound, target_bound, order.expression_, node_depth + 1);
            }
            if (sub_node.query_ != no_db_node) {
                rewrite_subquery(
                    query_storage.queries_[sub_node.query_].storage(), source_bound, target_bound, node_depth + 1);
            }
            if (!continues_logical_chain(query_storage.nodes_, sub_node, sub_node.left_)) {
                ++node_depth;
            }
            node = sub_node.left_;
        }
    }

    void rewrite_subquery_source(db_query_storage& query_storage, node_visit_set& visited,
        bool outer_source_bound, bool outer_target_bound, bool source_bound, bool target_bound,
        const db_query_source& src, std::size_t subquery_depth) {
        rewrite_subquery_node(
            query_storage, visited, source_bound, target_bound, src.expression_, subquery_depth + 1);
        if (src.query_ != no_db_node) {
            rewrite_subquery(query_storage.queries_[src.query_].storage(),
                src.lateral_ ? source_bound : outer_source_bound,
                src.lateral_ ? target_bound : outer_target_bound, subquery_depth + 1);
        }
    }

    void rewrite_subquery(db_query_storage& query_storage, bool outer_source_bound, bool outer_target_bound,
        std::size_t subquery_depth) {
        require_depth(subquery_depth);
        bool source_bound = outer_source_bound;
        bool target_bound = outer_target_bound;
        if (query_storage.source_) {
            source_bound = source_bound || source_binds_name(*query_storage.source_, source_qualifier_);
            target_bound = target_bound || source_binds_name(*query_storage.source_, target_qualifier_);
        }
        for (const auto& join : query_storage.joins_) {
            source_bound = source_bound || source_binds_name(join.source_, source_qualifier_);
            target_bound = target_bound || source_binds_name(join.source_, target_qualifier_);
        }
        // Only reachable expressions participate: abandoned builder nodes cannot
        // introduce a capture or an otherwise invisible nested query.
        node_visit_set visited(query_storage.nodes_.size());
        for (const auto node : query_storage.projections_) {
            rewrite_subquery_node(query_storage, visited, source_bound, target_bound, node, subquery_depth + 1);
        }
        for (const auto node : query_storage.distinct_on_) {
            rewrite_subquery_node(query_storage, visited, source_bound, target_bound, node, subquery_depth + 1);
        }
        for (const auto node : query_storage.groups_) {
            rewrite_subquery_node(query_storage, visited, source_bound, target_bound, node, subquery_depth + 1);
        }
        for (const auto& order : query_storage.orders_) {
            rewrite_subquery_node(
                query_storage, visited, source_bound, target_bound, order.expression_, subquery_depth + 1);
        }
        rewrite_subquery_node(
            query_storage, visited, source_bound, target_bound, query_storage.predicate_, subquery_depth + 1);
        rewrite_subquery_node(
            query_storage, visited, source_bound, target_bound, query_storage.having_, subquery_depth + 1);
        for (const auto& row : query_storage.rows_) {
            for (const auto node : row) {
                rewrite_subquery_node(query_storage, visited, source_bound, target_bound, node, subquery_depth + 1);
            }
        }
        if (query_storage.source_) {
            rewrite_subquery_source(query_storage, visited, outer_source_bound, outer_target_bound, source_bound,
                target_bound, *query_storage.source_, subquery_depth);
        }
        for (const auto& join : query_storage.joins_) {
            rewrite_subquery_source(query_storage, visited, outer_source_bound, outer_target_bound, source_bound,
                target_bound, join.source_, subquery_depth);
            rewrite_subquery_node(query_storage, visited, source_bound, target_bound, join.on_, subquery_depth + 1);
        }
        // CTE bodies and set operands are siblings of this query's body:
        // its FROM bindings do not shadow their correlated references.
        for (const auto& cte : query_storage.ctes_) {
            rewrite_subquery(query_storage.queries_[cte.query_].storage(), outer_source_bound, outer_target_bound,
                subquery_depth + 1);
        }
        for (const auto& operation : query_storage.set_operations_) {
            rewrite_subquery(query_storage.queries_[operation.query_].storage(), outer_source_bound,
                outer_target_bound, subquery_depth + 1);
        }
    }

    [[nodiscard]] std::size_t import_node(std::size_t id, std::size_t depth) {
        require_depth(depth);
        if (mapped_[id] != no_db_node) {
            return mapped_[id];
        }
        if (!continues_logical_chain(source_nodes_, source_nodes_[id], source_nodes_[id].left_)) {
            return import_one(id, no_db_node, depth);
        }
        std::vector<std::size_t> spine;
        collect_logical_spine(source_nodes_, id, mapped_, spine);
        auto imported = import_node(source_nodes_[spine.back()].left_, depth + 1);
        for (auto link = spine.rbegin(); link != spine.rend(); ++link) {
            imported = import_one(*link, imported, depth);
        }
        return imported;
    }

private:
    // Copies one node; `imported_left` is its already imported chain operand.
    [[nodiscard]] std::size_t import_one(std::size_t id, std::size_t imported_left, std::size_t depth) {
        db_query_node node_value(source_nodes_[id]);
        if (mark_aliased_ && (node_value.kind_ == db_node_kind::column || node_value.kind_ == db_node_kind::star)) {
            node_value.explicit_alias_ = true;
        }
        if (!source_qualifier_.empty() && !node_value.explicit_alias_ &&
            (node_value.kind_ == db_node_kind::column || node_value.kind_ == db_node_kind::star) &&
            node_value.qualifier_ == source_qualifier_) {
            node_value.qualifier_ = target_qualifier_;
        }
        if (imported_left != no_db_node) {
            node_value.left_ = imported_left;
        } else if (node_value.left_ != no_db_node) {
            node_value.left_ = import_node(node_value.left_, depth + 1);
        }
        if (node_value.right_ != no_db_node) {
            node_value.right_ = import_node(node_value.right_, depth + 1);
        }
        for (auto& arg : node_value.args_) {
            arg = import_node(arg, depth + 1);
        }
        for (auto& arg : node_value.named_) {
            arg.expression_ = import_node(arg.expression_, depth + 1);
        }
        for (auto& order : node_value.orders_) {
            order.expression_ = import_node(order.expression_, depth + 1);
        }
        if (node_value.query_ != no_db_node) {
            auto snapshot = source_queries_[node_value.query_].clone();
            if (!source_qualifier_.empty()) {
                rewrite_subquery(snapshot.storage(), false, false, depth + 1);
            }
            node_value.query_ = target_queries_.size();
            target_queries_.push_back(std::move(snapshot));
        }
        const auto index = target_nodes_.size();
        target_nodes_.push_back(std::move(node_value));
        mapped_[id] = index;
        return index;
    }
};

}  // namespace ruvia::detail

namespace ruvia {

using detail::db_node_kind;
using detail::db_query_kind;
using detail::db_source_kind;
using detail::no_db_node;

namespace detail {

struct db_expr_tree final {
    db_expr_tree() = default;

    db_expr_tree(const db_expr_tree& other)
        : table_(other.table_),
          root_(other.root_) {
        nodes_.reserve(other.nodes_.size());
        for (const auto& node : other.nodes_) {
            nodes_.emplace_back(node);
        }
        queries_.reserve(other.queries_.size());
        for (const auto& query : other.queries_) {
            queries_.push_back(query.clone());
        }
    }

    std::vector<db_query_node> nodes_;
    std::vector<db_query> queries_;
    std::string table_;
    std::size_t root_{no_db_node};
};

namespace {

// The caller's table names the FROM item an owned expression is imported
// for; the table stored with the expression stands in only when the caller
// names none. Qualifiers of any other table stay untouched, as for columns.
[[nodiscard]] std::string_view import_source_qualifier(std::string_view stored_table, std::string_view caller_table) noexcept {
    return caller_table.empty() ? stored_table : caller_table;
}

// Borrows either an owned expression's compact tree or a query's node arena.
// Import callbacks keep each route's ownership rules without temporary arrays.
struct expression_nodes final {
    const std::vector<db_query_node>& nodes_;

    template <class arena_type>
    explicit expression_nodes(arena_type& arena)
        : nodes_(arena.nodes_) {}

    [[nodiscard]] static std::size_t push(std::vector<db_query_node>& nodes, db_query_node&& node) {
        nodes.push_back(std::move(node));
        return nodes.size() - 1;
    }

    [[nodiscard]] db_query_node value(const db_value& value) const {
        return db_query_node(db_node_kind::value, clone_db_value_for_resource(value));
    }

    [[nodiscard]] db_query_node named(db_node_kind kind, std::string_view text = {},
        std::string_view qualifier = {}, bool explicit_alias = false) const {
        if (kind != db_node_kind::star) {
            require_name(text, kind == db_node_kind::collate);
        }
        if (!qualifier.empty()) {
            require_name(qualifier, true);
        }
        db_query_node node(kind);
        node.text_ = text;
        node.qualifier_ = qualifier;
        node.explicit_alias_ = explicit_alias;
        return node;
    }

    template <class range_type, class import_type>
    [[nodiscard]] db_query_node arguments(db_node_kind kind, const range_type& args, import_type import_argument) const {
        db_query_node node(kind);
        node.args_.reserve(args.size());
        for (const auto& arg : args) {
            node.args_.push_back(import_argument(arg));
        }
        return node;
    }

    template <class range_type, class import_type>
    [[nodiscard]] db_query_node variadic(db_node_kind kind, const range_type& args,
        std::string_view function, import_type import_argument) const {
        require_variadic_arguments(args.size(), function);
        return arguments(kind, args, import_argument);
    }

    template <class range_type, class import_type>
    [[nodiscard]] db_query_node sql(std::span<const std::string_view> parts,
        const range_type& args, import_type import_argument) const {
        validate_db_sql_parts(parts, args.size());
        db_query_node node(db_node_kind::sql);
        node.named_.reserve(args.size());
        for (std::size_t i = 0; i < args.size(); ++i) {
            node.named_.emplace_back(parts[i], import_argument(args[i]));
        }
        node.text_ = parts.back();
        return node;
    }

    template <class range_type, class import_type>
    [[nodiscard]] db_query_node call(db_node_kind kind, std::string_view function,
        const range_type& args, import_type import_argument) const {
        require_name(function, true);
        auto node = arguments(kind, args, import_argument);
        node.text_ = function;
        return node;
    }

    template <class import_type>
    void orders(db_query_node& node, std::span<const db_order_term> order, import_type import_argument) const {
        node.orders_.reserve(order.size());
        for (const auto& term : order) {
            node.orders_.push_back({import_argument(term.expression_), term.direction_, term.nulls_});
        }
    }

    template <class range_type, class import_type>
    [[nodiscard]] db_query_node aggregate(std::string_view function, const range_type& args,
        bool distinct, std::span<const db_order_term> order, import_type import_argument) const {
        auto node = call(db_node_kind::aggregate, function, args, import_argument);
        node.flag_ = distinct;
        orders(node, order, import_argument);
        return node;
    }

    [[nodiscard]] db_query_node operands(db_node_kind kind, std::size_t left,
        std::size_t right = no_db_node) const {
        db_query_node node(kind);
        node.left_ = left;
        node.right_ = right;
        return node;
    }

    [[nodiscard]] db_query_node unary(db_unary_operator op, std::size_t operand) const {
        auto node = operands(db_node_kind::unary, operand);
        node.unary_ = op;
        return node;
    }

    [[nodiscard]] db_query_node binary(std::size_t left, db_binary_operator op, std::size_t right) const {
        if (op == db_binary_operator::equal || op == db_binary_operator::not_equal) {
            const auto is_null = [&](std::size_t index) {
                const auto& node = nodes_[index];
                return node.kind_ == db_node_kind::value && node.value_.type() == db_value_type::null;
            };
            const bool null_left = is_null(left);
            if (null_left || is_null(right)) {
                return unary(op == db_binary_operator::equal ? db_unary_operator::is_null : db_unary_operator::is_not_null,
                    null_left ? right : left);
            }
        }
        auto node = operands(db_node_kind::binary, left, right);
        node.binary_ = op;
        return node;
    }

    [[nodiscard]] db_query_node cast(std::size_t operand, const db_type_definition& type) const {
        auto node = operands(db_node_kind::cast, operand);
        node.type_ = db_stored_type(type);
        return node;
    }

    [[nodiscard]] db_query_node extract(db_date_part part, std::size_t operand) const {
        auto node = operands(db_node_kind::extract, operand);
        node.date_part_ = part;
        return node;
    }

    [[nodiscard]] db_query_node collate(std::size_t operand, std::string_view collation) const {
        auto node = named(db_node_kind::collate, collation);
        node.left_ = operand;
        return node;
    }

    [[nodiscard]] db_query_node between(std::size_t operand, std::size_t lower,
        std::size_t upper, bool negate) const {
        const std::array args{operand, lower, upper};
        auto node = arguments(db_node_kind::between, args, [](auto index) { return index; });
        node.flag_ = negate;
        return node;
    }

    template <class import_type, class otherwise_type>
    [[nodiscard]] db_query_node case_when(std::span<const db_case_branch> branches,
        import_type import_argument, otherwise_type otherwise) const {
        if (branches.empty()) {
            throw std::invalid_argument("CASE requires a branch");
        }
        db_query_node node(db_node_kind::case_value);
        node.args_.reserve(branches.size() * 2U);
        for (const auto& branch : branches) {
            if (branch.when_.empty()) {
                throw std::invalid_argument("CASE WHEN requires a nonempty condition");
            }
            node.args_.push_back(import_argument(branch.when_));
            node.args_.push_back(import_argument(branch.then_));
        }
        node.left_ = otherwise();
        return node;
    }

    template <class import_type>
    [[nodiscard]] db_query_node over(std::size_t function, const db_window_options& window, import_type import_argument) const {
        auto node = arguments(db_node_kind::window, window.partition_by_, import_argument);
        node.left_ = function;
        orders(node, window.order_by_, import_argument);
        node.frame_ = window.frame_;
        return node;
    }

    template <class import_type>
    [[nodiscard]] db_query_node within_group(std::size_t function,
        std::span<const db_order_term> order, import_type import_argument) const {
        if (order.empty()) {
            throw std::invalid_argument("WITHIN GROUP requires ordering");
        }
        auto node = operands(db_node_kind::within_group, function);
        orders(node, order, import_argument);
        return node;
    }

    [[nodiscard]] db_query_node subquery(db_node_kind kind, std::size_t query) const {
        db_query_node node(kind);
        node.query_ = query;
        return node;
    }
};

[[nodiscard]] std::size_t push_tree_node(db_expr_tree& tree, db_query_node&& node) {
    return expression_nodes::push(tree.nodes_, std::move(node));
}

[[nodiscard]] std::size_t push_tree_value(db_expr_tree& tree, const db_value& value) {
    return push_tree_node(tree, expression_nodes(tree).value(value));
}

[[nodiscard]] std::size_t push_tree_text_array(
    db_expr_tree& tree, std::span<const std::string_view> strings) {
    const auto array_idx = push_tree_node(tree, expression_nodes(tree).arguments(db_node_kind::array, strings,
                                                    [&](std::string_view item) { return push_tree_value(tree, db_value(item)); }));
    return push_tree_node(tree, expression_nodes(tree).cast(array_idx,
                                    db_type_definition{.data_type_ = db_data_type::text, .array_ = true}));
}

}  // namespace

db_owned_expression::db_owned_expression() noexcept = default;

db_owned_expression::db_owned_expression(const db_owned_expression& other)
    : kind_(other.kind_),
      aliased_(other.aliased_),
      borrowed_(other.borrowed_),
      text_(other.text_),
      table_(other.table_),
      scalar_(other.scalar_) {
    if (other.tree_) {
        tree_ = std::make_unique<db_expr_tree>(*other.tree_);
    }
}

db_owned_expression& db_owned_expression::operator=(const db_owned_expression& other) {
    if (this != &other) {
        *this = db_owned_expression(other);
    }
    return *this;
}

db_owned_expression::db_owned_expression(db_owned_expression&& other) noexcept
    : kind_(std::exchange(other.kind_, leaf_kind::empty)),
      aliased_(std::exchange(other.aliased_, false)),
      borrowed_(std::exchange(other.borrowed_, db_expression{})),
      text_(std::exchange(other.text_, std::string_view{})),
      table_(std::exchange(other.table_, std::string_view{})),
      scalar_(other.scalar_),
      tree_(std::move(other.tree_)) {}

db_owned_expression& db_owned_expression::operator=(db_owned_expression&& other) noexcept {
    if (this != &other) {
        kind_ = std::exchange(other.kind_, leaf_kind::empty);
        aliased_ = std::exchange(other.aliased_, false);
        borrowed_ = std::exchange(other.borrowed_, db_expression{});
        text_ = std::exchange(other.text_, std::string_view{});
        table_ = std::exchange(other.table_, std::string_view{});
        scalar_ = other.scalar_;
        tree_ = std::move(other.tree_);
    }
    return *this;
}

db_owned_expression::~db_owned_expression() = default;

db_expression db_owned_expression::import_into(db_query& target, std::string_view table, std::string_view alias) const {
    switch (kind_) {
        case leaf_kind::empty:
            return {};
        case leaf_kind::borrowed: {
            const auto source_table = import_source_qualifier(table_, table);
            if (!aliased_ && !source_table.empty() && !alias.empty()) {
                return target.import_expression(borrowed_, source_table, alias);
            }
            return target.import_expression(borrowed_);
        }
        case leaf_kind::null_value:
            return target.null_value();
        case leaf_kind::default_value:
            return target.default_value();
        case leaf_kind::star:
            return target.star();
        case leaf_kind::column: {
            const auto qualifier =
                (!aliased_ && !table_.empty() && table_ == table && !alias.empty()) ? alias : table_;
            return target.push_node(expression_nodes(target.storage()).named(db_node_kind::column, text_, qualifier, aliased_));
        }
        case leaf_kind::excluded:
            return target.excluded(text_);
        case leaf_kind::signed_value:
            return target.value(db_value(scalar_.signed_value_));
        case leaf_kind::unsigned_value:
            return target.value(db_value(scalar_.unsigned_value_));
        case leaf_kind::double_value:
            return target.value(db_value(scalar_.double_value_));
        case leaf_kind::bool_value:
            return target.value(db_value(scalar_.bool_value_));
        case leaf_kind::tree: {
            auto& target_storage = target.storage();
            const auto source_table = import_source_qualifier(tree_->table_, table);
            const bool rewrite = !aliased_ && !source_table.empty() && !alias.empty();
            const auto source_qualifier = rewrite ? source_table : std::string_view{};
            const auto target_qualifier = rewrite ? alias : std::string_view{};
            if (!source_qualifier.empty()) {
                require_name(source_qualifier, true);
                require_name(target_qualifier, true);
            }
            reserve_append(target_storage.nodes_, tree_->nodes_.size());
            reserve_append(target_storage.queries_, tree_->queries_.size());
            qualified_expression_importer importer(
                tree_->nodes_, tree_->queries_, target_storage, source_qualifier, target_qualifier);
            return db_expression(&target_storage, importer.import_node(tree_->root_, 0));
        }
    }
    return {};
}

db_owned_expression db_owned_expression::from_borrowed(
    db_expression expression, borrowed_text table, bool aliased) noexcept {
    db_owned_expression result;
    if (!expression.empty()) {
        result.kind_ = leaf_kind::borrowed;
        result.borrowed_ = expression;
        result.table_ = table.view();
        result.aliased_ = aliased;
    }
    return result;
}

db_owned_expression::tree_owner_type db_owned_expression::allocate_tree() {
    return std::make_unique<db_expr_tree>();
}

db_owned_expression db_owned_expression::from_tree(tree_owner_type tree) noexcept {
    db_owned_expression result;
    result.kind_ = leaf_kind::tree;
    result.tree_ = std::move(tree);
    return result;
}

std::size_t db_owned_expression::append_to_tree(db_expr_tree& target) const {
    switch (kind_) {
        case leaf_kind::empty:
            throw std::invalid_argument("cannot evaluate an empty database expression");
        case leaf_kind::borrowed: {
            if (borrowed_.empty() || borrowed_.node_ >= borrowed_.owner_->nodes_.size()) {
                throw std::invalid_argument("cannot import an empty database expression");
            }
            if (!aliased_ && !table_.empty() && target.table_.empty()) {
                target.table_ = table_;
            }
            qualified_expression_importer importer(
                *borrowed_.owner_, target.nodes_, target.queries_, aliased_);
            return importer.import_node(borrowed_.node_, 0);
        }
        case leaf_kind::null_value:
            return push_tree_value(target, db_value(nullptr));
        case leaf_kind::default_value:
            return push_tree_node(target, db_query_node(db_node_kind::default_value));
        case leaf_kind::star:
            return push_tree_node(target, db_query_node(db_node_kind::star));
        case leaf_kind::column: {
            return push_tree_node(target, expression_nodes(target).named(db_node_kind::column, text_, table_, aliased_));
        }
        case leaf_kind::excluded: {
            return push_tree_node(target, expression_nodes(target).named(db_node_kind::excluded, text_));
        }
        case leaf_kind::signed_value:
            return push_tree_value(target, db_value(scalar_.signed_value_));
        case leaf_kind::unsigned_value:
            return push_tree_value(target, db_value(scalar_.unsigned_value_));
        case leaf_kind::double_value:
            return push_tree_value(target, db_value(scalar_.double_value_));
        case leaf_kind::bool_value:
            return push_tree_value(target, db_value(scalar_.bool_value_));
        case leaf_kind::tree: {
            if (!aliased_ && !tree_->table_.empty() && target.table_.empty()) {
                target.table_ = tree_->table_;
            }
            const auto node_offset = target.nodes_.size();
            const auto query_offset = target.queries_.size();
            reserve_append(target.queries_, tree_->queries_.size());
            for (const auto& query : tree_->queries_) {
                target.queries_.push_back(query.clone());
            }
            reserve_append(target.nodes_, tree_->nodes_.size());
            for (const auto& node : tree_->nodes_) {
                db_query_node copy(node);
                if (copy.left_ != no_db_node) {
                    copy.left_ += node_offset;
                }
                if (copy.right_ != no_db_node) {
                    copy.right_ += node_offset;
                }
                for (auto& arg : copy.args_) {
                    arg += node_offset;
                }
                for (auto& named : copy.named_) {
                    named.expression_ += node_offset;
                }
                for (auto& order : copy.orders_) {
                    order.expression_ += node_offset;
                }
                if (copy.query_ != no_db_node) {
                    copy.query_ += query_offset;
                }
                target.nodes_.push_back(std::move(copy));
            }
            return tree_->root_ + node_offset;
        }
    }
    return no_db_node;
}

db_owned_expression db_owned_expression::from_expression(
    db_expression expression, std::string_view table, bool aliased) {
    if (expression.empty() || expression.node_ >= expression.owner_->nodes_.size()) {
        throw std::invalid_argument("cannot construct an owned expression from an empty expression");
    }
    if (!table.empty()) {
        require_name(table, true);
    }
    auto tree = allocate_tree();
    if (!aliased) {
        tree->table_ = table;
    }
    qualified_expression_importer importer(
        *expression.owner_, tree->nodes_, tree->queries_, aliased);
    tree->root_ = importer.import_node(expression.node_, 0);
    auto result = from_tree(std::move(tree));
    result.aliased_ = aliased;
    return result;
}

db_owned_expression db_owned_expression::make_null() noexcept {
    db_owned_expression result;
    result.kind_ = leaf_kind::null_value;
    return result;
}

db_owned_expression db_owned_expression::make_default() noexcept {
    db_owned_expression result;
    result.kind_ = leaf_kind::default_value;
    return result;
}

db_owned_expression db_owned_expression::make_star(std::string_view table) {
    if (table.empty()) {
        db_owned_expression result;
        result.kind_ = leaf_kind::star;
        return result;
    }
    auto tree = allocate_tree();
    tree->root_ = push_tree_node(*tree, expression_nodes(*tree).named(db_node_kind::star, {}, table));
    return from_tree(std::move(tree));
}

db_owned_expression db_owned_expression::make_column(
    std::string_view name, std::string_view table, bool explicit_alias) {
    auto tree = allocate_tree();
    tree->root_ = push_tree_node(*tree, expression_nodes(*tree).named(db_node_kind::column, name, table, explicit_alias));
    return from_tree(std::move(tree));
}

db_owned_expression db_owned_expression::make_static_column(
    std::string_view name, std::string_view table, std::string_view alias) {
    if (alias.empty()) {
        db_owned_expression result;
        result.kind_ = leaf_kind::column;
        result.text_ = name;
        result.table_ = table;
        result.aliased_ = false;
        return result;
    }
    return make_column(name, alias, true);
}

db_owned_expression db_owned_expression::make_excluded(std::string_view name) {
    auto tree = allocate_tree();
    tree->root_ = push_tree_node(*tree, expression_nodes(*tree).named(db_node_kind::excluded, name));
    return from_tree(std::move(tree));
}

db_owned_expression db_owned_expression::make_static_excluded(std::string_view name) noexcept {
    db_owned_expression result;
    result.kind_ = leaf_kind::excluded;
    result.text_ = name;
    return result;
}

db_owned_expression db_owned_expression::make_value(const db_value& value) {
    db_owned_expression result;
    switch (value.type()) {
        case db_value_type::null:
            result.kind_ = leaf_kind::null_value;
            return result;
        case db_value_type::signed_value:
            result.kind_ = leaf_kind::signed_value;
            result.scalar_.signed_value_ = value.signed_value();
            return result;
        case db_value_type::unsigned_value:
            result.kind_ = leaf_kind::unsigned_value;
            result.scalar_.unsigned_value_ = value.unsigned_value();
            return result;
        case db_value_type::double_value:
            result.kind_ = leaf_kind::double_value;
            result.scalar_.double_value_ = value.double_value();
            return result;
        case db_value_type::bool_value:
            result.kind_ = leaf_kind::bool_value;
            result.scalar_.bool_value_ = value.bool_value();
            return result;
        case db_value_type::string: {
            auto tree = allocate_tree();
            tree->root_ = push_tree_value(*tree, value);
            return from_tree(std::move(tree));
        }
    }
    return result;
}

db_owned_expression db_owned_expression::make_raw_sql(
    std::span<const std::string_view> parts, std::span<const db_expr> args) {
    auto tree = allocate_tree();
    tree->nodes_.reserve(args.size() + 1U);
    tree->root_ = push_tree_node(*tree, expression_nodes(*tree).sql(parts, args,
                                            [&](const auto& arg) { return arg.owned().append_to_tree(*tree); }));
    return from_tree(std::move(tree));
}

db_owned_expression db_owned_expression::make_call(
    std::string_view function, std::span<const db_expr> args) {
    auto tree = allocate_tree();
    tree->nodes_.reserve(args.size() + 1U);
    tree->root_ = push_tree_node(*tree, expression_nodes(*tree).call(db_node_kind::function, function, args,
                                            [&](const auto& arg) { return arg.owned().append_to_tree(*tree); }));
    return from_tree(std::move(tree));
}

db_owned_expression db_owned_expression::make_variadic(
    db_node_kind kind, std::span<const db_expr> args, std::string_view function) {
    auto tree = allocate_tree();
    tree->nodes_.reserve(args.size() + 1U);
    tree->root_ = push_tree_node(*tree, expression_nodes(*tree).variadic(kind, args, function,
                                            [&](const auto& arg) { return arg.owned().append_to_tree(*tree); }));
    return from_tree(std::move(tree));
}

db_owned_expression db_owned_expression::make_two_operand_node(
    db_node_kind kind, const db_owned_expression& lhs, const db_owned_expression& rhs) {
    auto tree = allocate_tree();
    tree->nodes_.reserve(3U);
    const auto left_idx = lhs.append_to_tree(*tree);
    const auto right_idx = rhs.append_to_tree(*tree);
    tree->root_ = push_tree_node(*tree, expression_nodes(*tree).operands(kind, left_idx, right_idx));
    return from_tree(std::move(tree));
}

db_owned_expression db_owned_expression::make_coalesce(std::span<const db_expr> args) {
    return make_variadic(db_node_kind::coalesce, args, "COALESCE");
}

db_owned_expression db_owned_expression::make_null_if(
    const db_owned_expression& lhs, const db_owned_expression& rhs) {
    auto tree = allocate_tree();
    tree->nodes_.reserve(3U);
    const auto left_idx = lhs.append_to_tree(*tree);
    const auto right_idx = rhs.append_to_tree(*tree);
    const std::array args{left_idx, right_idx};
    tree->root_ = push_tree_node(*tree, expression_nodes(*tree).arguments(db_node_kind::null_if, args,
                                            [](auto index) { return index; }));
    return from_tree(std::move(tree));
}

db_owned_expression db_owned_expression::make_greatest(std::span<const db_expr> args) {
    return make_variadic(db_node_kind::greatest, args, "GREATEST");
}

db_owned_expression db_owned_expression::make_least(std::span<const db_expr> args) {
    return make_variadic(db_node_kind::least, args, "LEAST");
}

db_owned_expression db_owned_expression::make_case_when(
    std::span<const db_case_branch> branches, const db_owned_expression& otherwise) {
    auto tree = allocate_tree();
    tree->nodes_.reserve(branches.size() * 2U + 2U);
    tree->root_ = push_tree_node(*tree, expression_nodes(*tree).case_when(branches, [&](const auto& arg) { return arg.owned().append_to_tree(*tree); }, [&] { return otherwise.empty() ? no_db_node : otherwise.append_to_tree(*tree); }));
    return from_tree(std::move(tree));
}

db_owned_expression db_owned_expression::make_jsonb_set(const db_owned_expression& target,
    std::span<const std::string_view> path, const db_owned_expression& new_value, bool create_if_missing) {
    auto tree = allocate_tree();
    tree->nodes_.reserve(path.size() + 6U);
    const auto target_idx = target.append_to_tree(*tree);
    const auto path_idx = push_tree_text_array(*tree, path);
    const auto val_idx = new_value.append_to_tree(*tree);
    const auto flag_idx = push_tree_value(*tree, db_value(create_if_missing));
    const std::array args{target_idx, path_idx, val_idx, flag_idx};
    tree->root_ = push_tree_node(*tree, expression_nodes(*tree).call(db_node_kind::function, "jsonb_set", args,
                                            [](auto index) { return index; }));
    return from_tree(std::move(tree));
}

db_owned_expression db_owned_expression::make_json_path_op(
    const db_owned_expression& target, db_binary_operator op, std::span<const std::string_view> path) {
    auto tree = allocate_tree();
    tree->nodes_.reserve(path.size() + 4U);
    const auto target_idx = target.append_to_tree(*tree);
    const auto path_idx = push_tree_text_array(*tree, path);
    tree->root_ = push_tree_node(*tree, expression_nodes(*tree).binary(target_idx, op, path_idx));
    return from_tree(std::move(tree));
}

db_owned_expression db_owned_expression::make_aggregate(std::string_view function,
    std::span<const db_expr> args, bool distinct, std::span<const db_order_term> order) {
    auto tree = allocate_tree();
    tree->nodes_.reserve(args.size() + order.size() + 1U);
    tree->root_ = push_tree_node(*tree, expression_nodes(*tree).aggregate(function, args, distinct, order,
                                            [&](const auto& arg) { return arg.owned().append_to_tree(*tree); }));
    return from_tree(std::move(tree));
}

db_owned_expression db_owned_expression::make_count_star() {
    auto tree = allocate_tree();
    tree->nodes_.reserve(2U);
    const auto star_idx = push_tree_node(*tree, db_query_node(db_node_kind::star));
    const std::array args{star_idx};
    tree->root_ = push_tree_node(*tree, expression_nodes(*tree).call(db_node_kind::aggregate, "count", args,
                                            [](auto index) { return index; }));
    return from_tree(std::move(tree));
}

db_owned_expression db_owned_expression::make_filter(
    const db_owned_expression& aggregate, const db_owned_expression& predicate) {
    return make_two_operand_node(db_node_kind::filter, aggregate, predicate);
}

db_owned_expression db_owned_expression::make_over(
    const db_owned_expression& function, const db_window_options& window) {
    auto tree = allocate_tree();
    tree->nodes_.reserve(window.partition_by_.size() + window.order_by_.size() + 2U);
    const auto fn_idx = function.append_to_tree(*tree);
    tree->root_ = push_tree_node(*tree, expression_nodes(*tree).over(fn_idx, window,
                                            [&](const auto& arg) { return arg.owned().append_to_tree(*tree); }));
    return from_tree(std::move(tree));
}

db_owned_expression db_owned_expression::make_within_group(
    const db_owned_expression& function, std::span<const db_order_term> order) {
    auto tree = allocate_tree();
    tree->nodes_.reserve(order.size() + 2U);
    const auto fn_idx = function.append_to_tree(*tree);
    tree->root_ = push_tree_node(*tree, expression_nodes(*tree).within_group(fn_idx, order,
                                            [&](const auto& arg) { return arg.owned().append_to_tree(*tree); }));
    return from_tree(std::move(tree));
}

db_owned_expression db_owned_expression::make_cast(
    const db_owned_expression& operand, const db_type_definition& type) {
    auto tree = allocate_tree();
    tree->nodes_.reserve(2U);
    const auto op_idx = operand.append_to_tree(*tree);
    tree->root_ = push_tree_node(*tree, expression_nodes(*tree).cast(op_idx, type));
    return from_tree(std::move(tree));
}

db_owned_expression db_owned_expression::make_at_time_zone(
    const db_owned_expression& operand, const db_owned_expression& zone) {
    return make_two_operand_node(db_node_kind::at_time_zone, operand, zone);
}

db_owned_expression db_owned_expression::make_extract(
    db_date_part part, const db_owned_expression& operand) {
    auto tree = allocate_tree();
    tree->nodes_.reserve(2U);
    const auto op_idx = operand.append_to_tree(*tree);
    tree->root_ = push_tree_node(*tree, expression_nodes(*tree).extract(part, op_idx));
    return from_tree(std::move(tree));
}

db_owned_expression db_owned_expression::make_subscript(
    const db_owned_expression& array, const db_owned_expression& index) {
    return make_two_operand_node(db_node_kind::subscript, array, index);
}

db_owned_expression db_owned_expression::make_collate(
    const db_owned_expression& operand, std::string_view collation) {
    auto tree = allocate_tree();
    tree->nodes_.reserve(2U);
    const auto op_idx = operand.append_to_tree(*tree);
    tree->root_ = push_tree_node(*tree, expression_nodes(*tree).collate(op_idx, collation));
    return from_tree(std::move(tree));
}

db_owned_expression db_owned_expression::make_binary(
    const db_owned_expression& lhs, db_binary_operator op, const db_owned_expression& rhs) {
    const auto lhs_nodes = (lhs.kind_ == leaf_kind::tree && lhs.tree_ != nullptr) ? lhs.tree_->nodes_.size() : 1U;
    const auto rhs_nodes = (rhs.kind_ == leaf_kind::tree && rhs.tree_ != nullptr) ? rhs.tree_->nodes_.size() : 1U;
    auto tree = allocate_tree();
    tree->nodes_.reserve(lhs_nodes + rhs_nodes + 1U);
    const auto left_idx = lhs.append_to_tree(*tree);
    const auto right_idx = rhs.append_to_tree(*tree);
    tree->root_ = push_tree_node(*tree, expression_nodes(*tree).binary(left_idx, op, right_idx));
    return from_tree(std::move(tree));
}

db_owned_expression db_owned_expression::make_binary(
    db_owned_expression&& lhs, db_binary_operator op, const db_owned_expression& rhs) {
    if (&lhs != &rhs && lhs.kind_ == leaf_kind::tree && lhs.tree_ != nullptr) {
        const auto rhs_nodes = (rhs.kind_ == leaf_kind::tree && rhs.tree_ != nullptr) ? rhs.tree_->nodes_.size() : 1U;
        reserve_append(lhs.tree_->nodes_, rhs_nodes + 1U);
        const auto left_idx = lhs.tree_->root_;
        const auto right_idx = rhs.append_to_tree(*lhs.tree_);
        lhs.tree_->root_ = push_tree_node(*lhs.tree_, expression_nodes(*lhs.tree_).binary(left_idx, op, right_idx));
        lhs.aliased_ = false;
        return std::move(lhs);
    }
    return make_binary(std::as_const(lhs), op, rhs);
}

db_owned_expression db_owned_expression::make_unary(
    db_unary_operator op, const db_owned_expression& operand) {
    const auto operand_nodes =
        (operand.kind_ == leaf_kind::tree && operand.tree_ != nullptr) ? operand.tree_->nodes_.size() : 1U;
    auto tree = allocate_tree();
    tree->nodes_.reserve(operand_nodes + 1U);
    const auto op_idx = operand.append_to_tree(*tree);
    tree->root_ = push_tree_node(*tree, expression_nodes(*tree).unary(op, op_idx));
    return from_tree(std::move(tree));
}

db_owned_expression db_owned_expression::make_unary(
    db_unary_operator op, db_owned_expression&& operand) {
    if (operand.kind_ == leaf_kind::tree && operand.tree_ != nullptr) {
        const auto op_idx = operand.tree_->root_;
        operand.tree_->root_ = push_tree_node(*operand.tree_, expression_nodes(*operand.tree_).unary(op, op_idx));
        operand.aliased_ = false;
        return std::move(operand);
    }
    return make_unary(op, std::as_const(operand));
}

db_owned_expression db_owned_expression::make_between(const db_owned_expression& operand,
    const db_value& lower, const db_value& upper, bool negate) {
    auto tree = allocate_tree();
    tree->nodes_.reserve(4U);
    const auto val_idx = operand.append_to_tree(*tree);
    const auto low_idx = push_tree_value(*tree, lower);
    const auto high_idx = push_tree_value(*tree, upper);
    tree->root_ = push_tree_node(*tree, expression_nodes(*tree).between(val_idx, low_idx, high_idx, negate));
    return from_tree(std::move(tree));
}

db_owned_expression db_owned_expression::make_tuple(std::span<const db_expr> elements) {
    if (elements.empty()) {
        throw std::invalid_argument("database tuple requires at least one element");
    }
    auto tree = allocate_tree();
    tree->nodes_.reserve(elements.size() + 1U);
    tree->root_ = push_tree_node(*tree, expression_nodes(*tree).arguments(db_node_kind::tuple, elements,
                                            [&](const auto& arg) { return arg.owned().append_to_tree(*tree); }));
    return from_tree(std::move(tree));
}

db_owned_expression db_owned_expression::make_in_list(const db_owned_expression& lhs,
    db_binary_operator op, std::size_t count, const void* context,
    db_value (*element_at)(const void* context, std::size_t index)) {
    auto tree = allocate_tree();
    tree->nodes_.reserve(count + 3U);
    const auto left_idx = lhs.append_to_tree(*tree);
    const auto list_idx = push_tree_node(*tree, expression_nodes(*tree).arguments(db_node_kind::list,
                                                    std::views::iota(std::size_t{0}, count),
                                                    [&](std::size_t index) { return push_tree_value(*tree, element_at(context, index)); }));
    tree->root_ = push_tree_node(*tree, expression_nodes(*tree).binary(left_idx, op, list_idx));
    return from_tree(std::move(tree));
}

db_owned_expression db_owned_expression::make_in_expr_list(
    const db_owned_expression& lhs, db_binary_operator op, std::span<const db_tuple> values) {
    auto tree = allocate_tree();
    tree->nodes_.reserve(values.size() + 3U);
    const auto left_idx = lhs.append_to_tree(*tree);
    const auto list_idx = push_tree_node(*tree, expression_nodes(*tree).arguments(db_node_kind::list, values,
                                                    [&](const auto& value) { return value.owned_.append_to_tree(*tree); }));
    tree->root_ = push_tree_node(*tree, expression_nodes(*tree).binary(left_idx, op, list_idx));
    return from_tree(std::move(tree));
}

db_owned_expression db_owned_expression::make_in_subquery(
    const db_owned_expression& lhs, db_binary_operator op, const db_query& subquery) {
    subquery.require_select_query();
    auto tree = allocate_tree();
    tree->nodes_.reserve(3U);
    const auto left_idx = lhs.append_to_tree(*tree);
    const auto query_idx = tree->queries_.size();
    tree->queries_.push_back(subquery.clone());
    const auto sub_idx = push_tree_node(*tree, expression_nodes(*tree).subquery(db_node_kind::subquery, query_idx));
    tree->root_ = push_tree_node(*tree, expression_nodes(*tree).binary(left_idx, op, sub_idx));
    return from_tree(std::move(tree));
}

db_owned_expression db_owned_expression::make_exists(const db_query& subquery) {
    subquery.require_select_query();
    auto tree = allocate_tree();
    const auto query_idx = tree->queries_.size();
    tree->queries_.push_back(subquery.clone());
    tree->root_ = push_tree_node(*tree, expression_nodes(*tree).subquery(db_node_kind::exists, query_idx));
    return from_tree(std::move(tree));
}

db_owned_expression db_owned_expression::make_array_op(const db_owned_expression& lhs,
    db_binary_operator op, std::size_t count, const void* context,
    db_value (*element_at)(const void* context, std::size_t index),
    const db_type_definition& array_type) {
    auto tree = allocate_tree();
    tree->nodes_.reserve(count + 4U);
    const auto left_idx = lhs.append_to_tree(*tree);
    const auto array_idx = push_tree_node(*tree, expression_nodes(*tree).arguments(db_node_kind::array,
                                                     std::views::iota(std::size_t{0}, count),
                                                     [&](std::size_t index) { return push_tree_value(*tree, element_at(context, index)); }));
    const auto cast_idx = push_tree_node(*tree, expression_nodes(*tree).cast(array_idx, array_type));
    tree->root_ = push_tree_node(*tree, expression_nodes(*tree).binary(left_idx, op, cast_idx));
    return from_tree(std::move(tree));
}

}  // namespace detail

void db_query::storage_deleter_type::operator()(detail::db_query_storage* storage) const noexcept {
    delete storage;
}

db_query::db_query()
    : storage_(new detail::db_query_storage(), storage_deleter_type{}) {}
db_query::db_query(storage_owner_type storage) noexcept
    : storage_(std::move(storage)) {}
db_query::db_query(db_query&&) noexcept = default;
db_query& db_query::operator=(db_query&&) noexcept = default;
db_query::~db_query() = default;

detail::db_query_storage& db_query::storage() {
    if (!storage_) {
        throw std::logic_error("database query was moved from");
    }
    return *storage_;
}
const detail::db_query_storage& db_query::storage() const {
    if (!storage_) {
        throw std::logic_error("database query was moved from");
    }
    return *storage_;
}
bool db_query::returns_rows() const {
    return storage().returns_rows();
}
void db_query::require_select_query() const {
    if (has_writes()) {
        throw std::invalid_argument("subqueries require SELECT or VALUES; use a CTE for DML RETURNING");
    }
}
bool db_query::has_writes() const {
    const auto& s = storage();
    // Every source/expression subquery passes require_select_query(). Only
    // active CTEs and set operands can carry writes; queries_ also owns retired
    // sources and incomplete appends, which must not classify the current SQL.
    return (s.kind_ != db_query_kind::select && s.kind_ != db_query_kind::values) ||
           std::ranges::any_of(s.ctes_, [&](const auto& cte) { return s.queries_.at(cte.query_).has_writes(); }) ||
           std::ranges::any_of(s.set_operations_, [&](const auto& operand) { return s.queries_.at(operand.query_).has_writes(); });
}
bool db_query::has_where() const {
    return storage().predicate_ != no_db_node;
}
bool db_query::has_grouping() const {
    return !storage().groups_.empty() || storage().having_ != no_db_node;
}
std::size_t db_query::require_expression(db_expression expression) const {
    if (expression.owner_ != storage_.get() || expression.node_ >= storage().nodes_.size()) {
        throw std::invalid_argument("database expression belongs to another query; import it first");
    }
    return expression.node_;
}

detail::db_query_storage::db_query_storage(const db_query_storage& source)
    : nodes_(source.nodes_),
      kind_(source.kind_),
      target_(source.target_),
      target_alias_(source.target_alias_),
      columns_(source.columns_),
      projections_(source.projections_),
      groups_(source.groups_),
      orders_(source.orders_),
      distinct_(source.distinct_),
      distinct_on_(source.distinct_on_),
      returning_(source.returning_),
      source_(source.source_),
      joins_(source.joins_),
      rows_(source.rows_),
      assignments_(source.assignments_),
      insert_query_(source.insert_query_),
      predicate_(source.predicate_),
      having_(source.having_),
      limit_(source.limit_),
      offset_(source.offset_),
      conflict_(source.conflict_),
      lock_(source.lock_),
      ctes_(source.ctes_),
      set_operations_(source.set_operations_),
      cache_enabled_(source.cache_enabled_),
      cache_duration_(source.cache_duration_),
      cache_id_(source.cache_id_),
      cache_shape_(source.cache_shape_) {
    queries_.reserve(source.queries_.size());
    for (const auto& query_value : source.queries_) {
        queries_.push_back(query_value.clone());
    }
}

db_query::storage_owner_type db_query::copy_storage(const detail::db_query_storage& source) {
    return storage_owner_type(new detail::db_query_storage(source), storage_deleter_type{});
}
db_query db_query::clone() const {
    return db_query(copy_storage(storage()));
}

bool db_query::cacheable() const {
    const auto& s = storage();
    if ((s.kind_ != db_query_kind::select && s.kind_ != db_query_kind::values) || s.lock_) {
        return false;
    }
    const detail::reachable_query_graph graph(s);
    for (std::size_t index = 0; index < s.queries_.size(); ++index) {
        if (graph.query(index) != no_db_node && !s.queries_[index].cacheable()) {
            return false;
        }
    }
    return true;
}
std::optional<bool> db_query::cache_enabled() const {
    return storage().cache_enabled_;
}
std::optional<std::chrono::milliseconds> db_query::cache_duration() const {
    return storage().cache_duration_;
}
std::string_view db_query::cache_id() const {
    return storage().cache_id_;
}
detail::db_cache_shape db_query::cache_shape() const {
    return storage().cache_shape_;
}
void db_query::copy_cache(const db_query& source_value, detail::db_cache_shape shape) {
    auto& s = storage();
    s.cache_enabled_ = source_value.cache_enabled();
    s.cache_duration_ = source_value.cache_duration();
    s.cache_id_ = source_value.cache_id();
    s.cache_shape_ = shape;
}
db_query& db_query::cache(const db_cache_setting_type& setting) {
    return std::visit([&](const auto& value) -> db_query& {
        using t_type = std::remove_cvref_t<decltype(value)>;
        if constexpr (std::same_as<t_type, db_cache_options>) {
            return cache(std::string_view(value.id_), value.duration_);
        } else if constexpr (std::same_as<t_type, std::chrono::milliseconds>) {
            return cache(std::string_view{}, value);
        } else {
            auto& s = storage();
            s.cache_enabled_.reset();
            if constexpr (std::same_as<t_type, bool>) {
                s.cache_enabled_ = value;
            }
            s.cache_duration_.reset();
            s.cache_id_.clear();
            return *this;
        }
    },
        setting);
}
db_query& db_query::cache(std::string_view id, std::optional<std::chrono::milliseconds> duration) {
    if (duration && duration->count() <= 0) {
        throw std::invalid_argument("cache duration must be positive");
    }
    auto& s = storage();
    s.cache_id_ = id;
    s.cache_duration_ = duration;
    s.cache_enabled_ = true;
    return *this;
}

detail::db_query_node db_query::make_node(db_node_kind kind, std::span<const db_expression> args) const {
    return detail::expression_nodes(storage()).arguments(kind, args,
        [&](db_expression arg) { return require_expression(arg); });
}
db_expression db_query::push_node(detail::db_query_node&& node_value) {
    auto& s = storage();
    return db_expression(&s, detail::expression_nodes::push(s.nodes_, std::move(node_value)));
}

db_expression db_query::column(std::string_view name, std::string_view table_value) {
    return push_node(detail::expression_nodes(storage()).named(db_node_kind::column, name, table_value));
}
db_expression db_query::star(std::string_view table_value) {
    return push_node(detail::expression_nodes(storage()).named(db_node_kind::star, {}, table_value));
}
db_expression db_query::value(const db_value& value) {
    return push_node(detail::expression_nodes(storage()).value(value));
}
db_expression db_query::null_value() {
    return value(db_value(nullptr));
}
db_expression db_query::excluded(std::string_view column) {
    return push_node(detail::expression_nodes(storage()).named(db_node_kind::excluded, column));
}
db_expression db_query::default_value() {
    return push_node(make_node(db_node_kind::default_value));
}
db_expression db_query::sql(std::span<const std::string_view> parts, std::span<const db_expression> args) {
    return push_node(detail::expression_nodes(storage()).sql(parts, args,
        [&](db_expression arg) { return require_expression(arg); }));
}
db_expression db_query::call(std::string_view function, std::span<const db_expression> args, std::span<const db_named_argument> named) {
    auto node_value = detail::expression_nodes(storage()).call(db_node_kind::function, function, args,
        [&](db_expression arg) { return require_expression(arg); });
    node_value.named_.reserve(named.size());
    for (const auto& arg : named) {
        detail::require_name(arg.name_);
        if (std::ranges::any_of(node_value.named_, [&](const auto& existing) { return std::string_view(existing.name_) == arg.name_; })) {
            throw std::invalid_argument("duplicate named database function argument");
        }
        node_value.named_.emplace_back(arg.name_, require_expression(arg.value_));
    }
    return push_node(std::move(node_value));
}
db_expression db_query::coalesce(std::span<const db_expression> args) {
    return push_node(detail::expression_nodes(storage()).variadic(db_node_kind::coalesce, args, "COALESCE",
        [&](db_expression arg) { return require_expression(arg); }));
}
db_expression db_query::null_if(db_expression value, db_expression other) {
    const std::array args{value, other};
    return push_node(make_node(db_node_kind::null_if, args));
}
db_expression db_query::greatest(std::span<const db_expression> args) {
    return push_node(detail::expression_nodes(storage()).variadic(db_node_kind::greatest, args, "GREATEST",
        [&](db_expression arg) { return require_expression(arg); }));
}
db_expression db_query::least(std::span<const db_expression> args) {
    return push_node(detail::expression_nodes(storage()).variadic(db_node_kind::least, args, "LEAST",
        [&](db_expression arg) { return require_expression(arg); }));
}
db_expression db_query::binary(db_expression lhs, db_binary_operator op, db_expression rhs) {
    const auto left = require_expression(lhs);
    const auto right = require_expression(rhs);
    return push_node(detail::expression_nodes(storage()).binary(left, op, right));
}
db_expression db_query::unary(db_unary_operator op, db_expression value) {
    return push_node(detail::expression_nodes(storage()).unary(op, require_expression(value)));
}
db_expression db_query::between(db_expression value, db_expression lower, db_expression upper, bool negate) {
    return push_node(detail::expression_nodes(storage()).between(
        require_expression(value), require_expression(lower), require_expression(upper), negate));
}
db_expression db_query::tuple(std::span<const db_expression> values) {
    return push_node(make_node(db_node_kind::tuple, values));
}
db_expression db_query::list(std::span<const db_expression> values) {
    return push_node(make_node(db_node_kind::list, values));
}
db_expression db_query::array(std::span<const db_expression> values) {
    return push_node(make_node(db_node_kind::array, values));
}
db_expression db_query::any(db_expression array_value) {
    return push_node(detail::expression_nodes(storage()).operands(db_node_kind::any, require_expression(array_value)));
}
db_expression db_query::all(db_expression array_value) {
    return push_node(detail::expression_nodes(storage()).operands(db_node_kind::all, require_expression(array_value)));
}
db_expression db_query::cast(db_expression value, const db_type_definition& type) {
    return push_node(detail::expression_nodes(storage()).cast(require_expression(value), type));
}
db_expression db_query::alias(db_expression value, std::string_view name) {
    detail::require_name(name);
    auto node_value = make_node(db_node_kind::alias);
    node_value.left_ = require_expression(value);
    node_value.text_ = name;
    return push_node(std::move(node_value));
}
db_expression db_query::case_when(std::span<const db_case_branch> branches, db_expression otherwise) {
    return push_node(detail::expression_nodes(storage()).case_when(branches, [&](const auto& arg) { return require_expression(arg.expression(*this)); }, [&] { return otherwise.empty() ? no_db_node : require_expression(otherwise); }));
}
db_query db_query::adopt(db_query&& query) const {
    if (&query == this) {
        return query.clone();
    }
    return std::move(query);
}
db_expression db_query::add_subquery(db_node_kind kind, db_query&& query) {
    query.require_select_query();
    auto& s = storage();
    auto node_value = detail::expression_nodes(s).subquery(kind, s.queries_.size());
    detail::reserve_append(s.nodes_, 1);
    detail::reserve_append(s.queries_, 1);
    s.queries_.push_back(adopt(std::move(query)));
    return push_node(std::move(node_value));
}
db_expression db_query::subquery(const db_query& query) {
    return subquery(query.clone());
}
db_expression db_query::subquery(db_query&& query) {
    return add_subquery(db_node_kind::subquery, std::move(query));
}
db_expression db_query::exists(const db_query& query) {
    return exists(query.clone());
}
db_expression db_query::exists(db_query&& query) {
    return add_subquery(db_node_kind::exists, std::move(query));
}
db_expression db_query::aggregate(std::string_view function, std::span<const db_expression> args, bool distinct, std::span<const db_order_term> order) {
    return push_node(detail::expression_nodes(storage()).aggregate(function, args, distinct, order,
        [&](const auto& arg) {
            if constexpr (std::same_as<std::remove_cvref_t<decltype(arg)>, db_expression>) {
                return require_expression(arg);
            } else {
                return require_expression(arg.expression(*this));
            }
        }));
}
db_expression db_query::filter(db_expression aggregate, db_expression predicate) {
    return push_node(detail::expression_nodes(storage()).operands(db_node_kind::filter,
        require_expression(aggregate), require_expression(predicate)));
}
db_expression db_query::over(db_expression function, const db_window_options& window) {
    const auto function_node = require_expression(function);
    return push_node(detail::expression_nodes(storage()).over(function_node, window,
        [&](const auto& arg) { return require_expression(arg.expression(*this)); }));
}
db_expression db_query::within_group(db_expression function, std::span<const db_order_term> order) {
    return push_node(detail::expression_nodes(storage()).within_group(require_expression(function), order,
        [&](const auto& arg) { return require_expression(arg.expression(*this)); }));
}
db_expression db_query::extract(db_date_part part, db_expression value) {
    return push_node(detail::expression_nodes(storage()).extract(part, require_expression(value)));
}
db_expression db_query::subscript(db_expression array_value, db_expression index) {
    return push_node(detail::expression_nodes(storage()).operands(db_node_kind::subscript,
        require_expression(array_value), require_expression(index)));
}
db_expression db_query::collate(db_expression value, std::string_view collation) {
    return push_node(detail::expression_nodes(storage()).collate(require_expression(value), collation));
}
db_expression db_query::at_time_zone(db_expression value, db_expression zone) {
    return push_node(detail::expression_nodes(storage()).operands(db_node_kind::at_time_zone,
        require_expression(value), require_expression(zone)));
}

db_expression db_query::import_expression(db_expression expression, std::string_view source_qualifier, std::string_view target_qualifier) {
    if (expression.empty() || expression.node_ >= expression.owner_->nodes_.size()) {
        throw std::invalid_argument("cannot import an empty database expression");
    }
    auto& target = storage();
    if (source_qualifier.empty() != target_qualifier.empty()) {
        throw std::invalid_argument("expression qualifier mapping requires both source and target");
    }
    if (!source_qualifier.empty()) {
        detail::require_name(source_qualifier, true);
        detail::require_name(target_qualifier, true);
    }
    if (expression.owner_ == &target && source_qualifier.empty()) {
        return expression;
    }
    detail::qualified_expression_importer importer(
        *expression.owner_, target, source_qualifier, target_qualifier);
    return db_expression(&target, importer.import_node(expression.node_, 0));
}

db_query& db_query::replace_expressions(
    std::vector<std::size_t>& target, std::span<const db_expression> expressions) {
    for (const auto expression : expressions) {
        (void)require_expression(expression);
    }
    target.reserve(expressions.size());
    target.clear();
    for (const auto expression : expressions) {
        target.push_back(expression.node_);
    }
    return *this;
}
db_query& db_query::combine_clause_predicate(
    std::size_t& clause, db_binary_operator op, db_expression predicate) {
    const auto previous = clause;
    clause = require_expression(
        previous == no_db_node ? predicate : binary(db_expression(storage_.get(), previous), op, predicate));
    return *this;
}
db_query& db_query::select(std::span<const db_expression> projections) {
    return replace_expressions(storage().projections_, projections);
}
db_query& db_query::add_select(db_expression projection) {
    storage().projections_.push_back(require_expression(projection));
    return *this;
}
db_query& db_query::from(std::string_view table_value, std::string_view alias) {
    detail::require_name(table_value, true);
    if (!alias.empty()) {
        detail::require_unqualified_name(alias);
    }
    auto& s = storage();
    detail::db_query_source source_value;
    source_value.name_ = table_value;
    source_value.alias_ = alias;
    s.source_ = std::move(source_value);
    return *this;
}
db_query& db_query::from(const db_query& query, std::string_view alias, const db_source_options& options) {
    return from(query.clone(), alias, options);
}
db_query& db_query::from(db_query&& query, std::string_view alias, const db_source_options& options) {
    detail::require_unqualified_name(alias);
    query.require_select_query();
    auto& s = storage();
    detail::db_query_source source_value;
    source_value.kind_ = db_source_kind::query;
    source_value.alias_ = alias;
    source_value.query_ = s.queries_.size();
    detail::set_source_options(source_value, options);
    detail::reserve_append(s.queries_, 1);
    s.queries_.push_back(adopt(std::move(query)));
    s.source_.emplace(std::move(source_value));
    return *this;
}
db_query& db_query::from_function(db_expression function, std::string_view alias, const db_source_options& options) {
    detail::require_unqualified_name(alias);
    const auto index = require_expression(function);
    auto& s = storage();
    detail::db_query_source source_value;
    source_value.kind_ = db_source_kind::function;
    source_value.expression_ = index;
    source_value.alias_ = alias;
    detail::set_source_options(source_value, options);
    s.source_ = std::move(source_value);
    return *this;
}
db_query& db_query::clear_source() {
    storage().source_.reset();
    return *this;
}
db_query& db_query::join(db_join_type type, std::string_view table_value, db_expression on, std::string_view alias, std::span<const std::string_view> using_columns) {
    detail::require_name(table_value, true);
    if (!alias.empty()) {
        detail::require_unqualified_name(alias);
    }
    auto& s = storage();
    detail::db_query_source source_value;
    source_value.name_ = table_value;
    source_value.alias_ = alias;
    detail::db_stored_join join(type, std::move(source_value), on.empty() ? no_db_node : require_expression(on));
    join.using_columns_.reserve(using_columns.size());
    for (const auto name : using_columns) {
        detail::require_name(name);
        join.using_columns_.emplace_back(name);
    }
    s.joins_.push_back(std::move(join));
    return *this;
}
db_query& db_query::join(db_join_type type, const db_query& query, db_expression on, std::string_view alias, const db_source_options& options) {
    return join(type, query.clone(), on, alias, options);
}
db_query& db_query::join(db_join_type type, db_query&& query, db_expression on, std::string_view alias, const db_source_options& options) {
    detail::require_unqualified_name(alias);
    const auto predicate = on.empty() ? no_db_node : require_expression(on);
    query.require_select_query();
    auto& s = storage();
    detail::db_query_source source_value;
    source_value.kind_ = db_source_kind::query;
    source_value.query_ = s.queries_.size();
    source_value.alias_ = alias;
    detail::set_source_options(source_value, options);
    detail::db_stored_join join_value(type, std::move(source_value), predicate);
    detail::reserve_append(s.queries_, 1);
    detail::reserve_append(s.joins_, 1);
    s.queries_.push_back(adopt(std::move(query)));
    s.joins_.push_back(std::move(join_value));
    return *this;
}
db_query& db_query::join_function(db_join_type type, db_expression function, db_expression on, std::string_view alias, const db_source_options& options) {
    detail::require_unqualified_name(alias);
    const auto function_id = require_expression(function);
    const auto predicate = on.empty() ? no_db_node : require_expression(on);
    auto& s = storage();
    detail::db_query_source source_value;
    source_value.kind_ = db_source_kind::function;
    source_value.expression_ = function_id;
    source_value.alias_ = alias;
    detail::set_source_options(source_value, options);
    s.joins_.emplace_back(type, std::move(source_value), predicate);
    return *this;
}
db_query& db_query::where(db_expression predicate) {
    storage().predicate_ = require_expression(predicate);
    return *this;
}
db_query& db_query::and_where(db_expression predicate) {
    return combine_clause_predicate(storage().predicate_, db_binary_operator::and_value, predicate);
}
db_query& db_query::or_where(db_expression predicate) {
    return combine_clause_predicate(storage().predicate_, db_binary_operator::or_value, predicate);
}
db_query& db_query::clear_where() {
    storage().predicate_ = no_db_node;
    return *this;
}
db_query& db_query::group_by(std::span<const db_expression> expressions) {
    return replace_expressions(storage().groups_, expressions);
}
db_query& db_query::add_group_by(db_expression expression) {
    storage().groups_.push_back(require_expression(expression));
    return *this;
}
db_query& db_query::having(db_expression predicate) {
    storage().having_ = require_expression(predicate);
    return *this;
}
db_query& db_query::and_having(db_expression predicate) {
    return combine_clause_predicate(storage().having_, db_binary_operator::and_value, predicate);
}
db_query& db_query::or_having(db_expression predicate) {
    return combine_clause_predicate(storage().having_, db_binary_operator::or_value, predicate);
}
db_query& db_query::clear_having() {
    storage().having_ = no_db_node;
    return *this;
}
db_query& db_query::order_by(db_expression expression, db_order_direction direction, db_nulls_order nulls) {
    const auto index = require_expression(expression);
    auto& s = storage();
    s.orders_.reserve(1);
    s.orders_.clear();
    s.orders_.push_back({index, direction, nulls});
    return *this;
}
db_query& db_query::add_order_by(db_expression expression, db_order_direction direction, db_nulls_order nulls) {
    storage().orders_.push_back({require_expression(expression), direction, nulls});
    return *this;
}
db_query& db_query::clear_order() {
    storage().orders_.clear();
    return *this;
}
db_query& db_query::limit(std::optional<query_row_count> count) {
    storage().limit_ = count ? std::optional<std::uint64_t>(count->value()) : std::nullopt;
    return *this;
}
db_query& db_query::set_limit_value(std::uint64_t count) {
    storage().limit_ = count;
    return *this;
}
db_query& db_query::offset(std::optional<query_row_count> count) {
    storage().offset_ = count ? std::optional<std::uint64_t>(count->value()) : std::nullopt;
    return *this;
}
db_query& db_query::set_offset_value(std::uint64_t count) {
    storage().offset_ = count;
    return *this;
}
db_query& db_query::distinct(bool enabled) {
    auto& s = storage();
    s.distinct_ = enabled;
    s.distinct_on_.clear();
    return *this;
}
db_query& db_query::distinct_on(std::span<const db_expression> expressions) {
    if (expressions.empty()) {
        throw std::invalid_argument("DISTINCT ON requires an expression");
    }
    auto& s = storage();
    (void)replace_expressions(s.distinct_on_, expressions);
    s.distinct_ = false;
    return *this;
}
db_query& db_query::returning(std::span<const db_expression> expressions) {
    return replace_expressions(storage().returning_, expressions);
}
db_query& db_query::insert_into(std::string_view table_value, std::span<const std::string_view> columns, std::string_view alias) {
    detail::require_name(table_value, true);
    if (!alias.empty()) {
        detail::require_unqualified_name(alias);
    }
    for (std::size_t i = 0; i < columns.size(); ++i) {
        detail::require_name(columns[i]);
        for (std::size_t j = 0; j < i; ++j) {
            if (columns[j] == columns[i]) {
                throw std::invalid_argument("duplicate INSERT column");
            }
        }
    }
    auto& s = storage();
    detail::require_statement_kind_change(s, db_query_kind::insert);
    if (s.kind_ == db_query_kind::insert && !s.rows_.empty() && !std::ranges::equal(s.columns_, columns)) {
        throw std::invalid_argument("INSERT rows already use a different column list");
    }
    std::string target(table_value);
    std::string target_alias(alias);
    std::vector<std::string> staged_columns;
    staged_columns.reserve(columns.size());
    for (const auto column : columns) {
        staged_columns.emplace_back(column);
    }
    s.target_.swap(target);
    s.target_alias_.swap(target_alias);
    s.columns_.swap(staged_columns);
    s.kind_ = db_query_kind::insert;
    return *this;
}
db_query& db_query::values(std::span<const db_expression> row) {
    auto& s = storage();
    if (s.kind_ != db_query_kind::select && s.kind_ != db_query_kind::values && s.kind_ != db_query_kind::insert) {
        throw std::invalid_argument("VALUES requires an INSERT or a values query");
    }
    std::vector<std::size_t> values;
    values.reserve(row.size());
    for (const auto expression : row) {
        values.push_back(require_expression(expression));
    }
    s.rows_.push_back(std::move(values));
    if (s.kind_ != db_query_kind::insert) {
        s.kind_ = db_query_kind::values;
    }
    return *this;
}
db_query& db_query::insert_rows(std::span<const std::string_view> columns,
    std::span<const db_expression> values, std::size_t row_count) {
    auto& s = storage();
    if (s.kind_ != db_query_kind::insert || s.insert_query_ != no_db_node) {
        throw std::invalid_argument("named INSERT rows require an INSERT without SELECT");
    }
    if (row_count == 0 ||
        (columns.empty() ? !values.empty()
                         : values.size() / columns.size() != row_count || values.size() % columns.size() != 0)) {
        throw std::invalid_argument("INSERT batch dimensions differ");
    }
    if (row_count > s.rows_.max_size() - s.rows_.size()) {
        throw std::length_error("INSERT has too many rows");
    }
    for (const auto& row : s.rows_) {
        if (row.size() != s.columns_.size()) {
            throw std::invalid_argument("existing INSERT rows do not match their named columns");
        }
    }
    for (const auto expression : values) {
        (void)require_expression(expression);
    }
    detail::node_index_map positions(columns.size());
    std::vector<std::string> new_columns;
    for (std::size_t index = 0; index < columns.size(); ++index) {
        detail::require_name(columns[index]);
        for (std::size_t earlier = 0; earlier < index; ++earlier) {
            if (columns[earlier] == columns[index]) {
                throw std::invalid_argument("duplicate INSERT column");
            }
        }
        const auto existing = std::ranges::find(s.columns_, columns[index]);
        if (existing != s.columns_.end()) {
            positions[index] = static_cast<std::size_t>(existing - s.columns_.begin());
        } else {
            if (new_columns.empty()) {
                new_columns = s.columns_;
            }
            positions[index] = new_columns.size();
            new_columns.emplace_back(columns[index]);
        }
    }
    const auto width = new_columns.empty() ? s.columns_.size() : new_columns.size();
    if (width == 0 && (row_count != 1 || !s.rows_.empty())) {
        throw std::invalid_argument("bulk default-only inserts require at least one explicit column");
    }
    const bool needs_default = width > columns.size() || (!s.rows_.empty() && width > s.columns_.size());
    const auto default_node = needs_default ? default_value().node_ : no_db_node;
    std::vector<std::vector<std::size_t>> rows;
    rows.reserve(row_count);
    for (std::size_t row_index = 0; row_index < row_count; ++row_index) {
        auto& row = rows.emplace_back();
        row.assign(width, default_node);
        for (std::size_t column = 0; column < columns.size(); ++column) {
            row[positions[column]] = values[row_index * columns.size() + column].node_;
        }
    }
    // Capacity changes are not visible query semantics. Complete every possible
    // allocation before extending old rows or publishing any new row.
    detail::reserve_append(s.rows_, row_count);
    if (!new_columns.empty()) {
        for (auto& row : s.rows_) {
            row.reserve(width);
        }
        for (auto& row : s.rows_) {
            row.resize(width, default_node);
        }
        s.columns_.swap(new_columns);
    }
    for (auto& row : rows) {
        s.rows_.push_back(std::move(row));
    }
    return *this;
}
db_query& db_query::insert_from(const db_query& query) {
    return insert_from(query.clone());
}
db_query& db_query::insert_from(db_query&& query) {
    if (storage().kind_ != db_query_kind::insert || !storage().rows_.empty()) {
        throw std::invalid_argument("INSERT SELECT requires an INSERT target without VALUES");
    }
    query.require_select_query();
    auto& s = storage();
    const auto index = s.queries_.size();
    detail::reserve_append(s.queries_, 1);
    s.queries_.push_back(adopt(std::move(query)));
    s.insert_query_ = index;
    return *this;
}
db_query& db_query::on_conflict(const db_conflict_options& conflict) {
    auto& s = storage();
    if (s.kind_ != db_query_kind::insert) {
        throw std::invalid_argument("ON CONFLICT requires an INSERT");
    }
    detail::db_stored_conflict owned;
    owned.columns_.reserve(conflict.columns_.size());
    for (const auto& name : conflict.columns_) {
        detail::require_name(name);
        owned.columns_.emplace_back(name);
    }
    if (!conflict.constraint_.empty()) {
        detail::require_name(conflict.constraint_);
        owned.constraint_ = conflict.constraint_;
    }
    owned.do_nothing_ = conflict.do_nothing_;
    owned.any_unique_key_ = conflict.any_unique_key_;
    if (!conflict.target_where_.empty()) {
        owned.target_where_ = require_expression(conflict.target_where_);
    }
    if (!conflict.update_where_.empty()) {
        owned.update_where_ = require_expression(conflict.update_where_);
    }
    owned.assignments_.reserve(conflict.update_.size());
    for (const auto& assignment : conflict.update_) {
        detail::require_name(assignment.column_);
        if (std::ranges::any_of(owned.assignments_, [&](const auto& existing) { return std::string_view(existing.column_) == assignment.column_; })) {
            throw std::invalid_argument("duplicate conflict update column");
        }
        owned.assignments_.emplace_back(assignment.column_, require_expression(assignment.expression(*this)));
    }
    s.conflict_ = std::move(owned);
    return *this;
}
db_query& db_query::update(std::string_view table_value, std::string_view alias) {
    detail::require_name(table_value, true);
    if (!alias.empty()) {
        detail::require_unqualified_name(alias);
    }
    auto& s = storage();
    detail::require_statement_kind_change(s, db_query_kind::update);
    s.kind_ = db_query_kind::update;
    s.target_ = table_value;
    s.target_alias_ = alias;
    return *this;
}
db_query& db_query::set(std::string_view column, db_expression value) {
    detail::require_name(column);
    const auto expression = require_expression(value);
    auto& s = storage();
    if (s.kind_ != db_query_kind::update) {
        throw std::invalid_argument("SET requires an UPDATE");
    }
    for (auto& existing : s.assignments_) {
        if (existing.column_ == column) {
            existing.expression_ = expression;
            return *this;
        }
    }
    s.assignments_.emplace_back(column, expression);
    return *this;
}
db_query& db_query::set(const db_assignment& assignment) {
    return set(assignment.column_, assignment.expression(*this));
}
db_query& db_query::set(std::span<const db_assignment> assignments) {
    if (assignments.size() == 1) {
        return set(assignments.front());
    }
    auto& s = storage();
    if (s.kind_ != db_query_kind::update) {
        throw std::invalid_argument("SET requires an UPDATE");
    }
    std::vector<detail::db_stored_assignment> prepared;
    prepared.reserve(assignments.size());
    detail::node_index_map positions(assignments.size());
    const auto previous_count = s.assignments_.size();
    std::size_t added = 0;
    for (const auto& assignment : assignments) {
        const auto column = assignment.column_.view();
        detail::require_name(column);
        const auto expression = require_expression(assignment.expression(*this));
        const auto previous = std::ranges::find(prepared, column, &detail::db_stored_assignment::column_);
        if (previous != prepared.end()) {
            previous->expression_ = expression;
            continue;
        }
        const auto existing = std::ranges::find(s.assignments_, column, &detail::db_stored_assignment::column_);
        if (existing != s.assignments_.end()) {
            positions[prepared.size()] = static_cast<std::size_t>(existing - s.assignments_.begin());
        } else {
            if (added == s.assignments_.max_size() - previous_count) {
                throw std::length_error("UPDATE has too many assignments");
            }
            positions[prepared.size()] = previous_count + added++;
        }
        prepared.emplace_back(column, expression);
    }
    s.assignments_.reserve(previous_count + added);
    for (std::size_t index = 0; index < prepared.size(); ++index) {
        if (positions[index] < previous_count) {
            s.assignments_[positions[index]].expression_ = prepared[index].expression_;
        } else {
            s.assignments_.push_back(std::move(prepared[index]));
        }
    }
    return *this;
}
db_query& db_query::update_from(std::string_view table_value, std::string_view alias) {
    if (storage().kind_ != db_query_kind::update) {
        throw std::invalid_argument("UPDATE FROM requires an UPDATE");
    }
    return from(table_value, alias);
}
db_query& db_query::update_from(const db_query& query, std::string_view alias) {
    if (storage().kind_ != db_query_kind::update) {
        throw std::invalid_argument("UPDATE FROM requires an UPDATE");
    }
    return from(query, alias);
}
db_query& db_query::delete_from(std::string_view table_value, std::string_view alias) {
    detail::require_name(table_value, true);
    if (!alias.empty()) {
        detail::require_unqualified_name(alias);
    }
    auto& s = storage();
    detail::require_statement_kind_change(s, db_query_kind::delete_value);
    s.kind_ = db_query_kind::delete_value;
    s.target_ = table_value;
    s.target_alias_ = alias;
    return *this;
}
db_query& db_query::delete_using(std::string_view table_value, std::string_view alias) {
    if (storage().kind_ != db_query_kind::delete_value) {
        throw std::invalid_argument("DELETE USING requires a DELETE");
    }
    return from(table_value, alias);
}
db_query& db_query::delete_using(const db_query& query, std::string_view alias) {
    if (storage().kind_ != db_query_kind::delete_value) {
        throw std::invalid_argument("DELETE USING requires a DELETE");
    }
    return from(query, alias);
}
db_query& db_query::lock(const db_lock_options& options) {
    if (options.nowait_ && options.skip_locked_) {
        throw std::invalid_argument("NOWAIT and SKIP LOCKED are mutually exclusive");
    }
    for (const auto& table : options.tables_) {
        detail::require_unqualified_name(table);
    }
    detail::db_stored_lock lock_value(options);
    storage().lock_.emplace(std::move(lock_value));
    return *this;
}
db_query& db_query::clear_lock() {
    storage().lock_.reset();
    return *this;
}
db_query& db_query::with(std::string_view name, const db_query& query, const db_cte_options& options) {
    return with(name, query.clone(), options);
}
db_query& db_query::with(std::string_view name, db_query&& query, const db_cte_options& options) {
    auto& s = storage();
    detail::validate_cte(s, name, options.columns_.view());
    auto cte = detail::prepare_cte(s, name, options.recursive_, options.materialization_, options.columns_.view());
    s.queries_.push_back(adopt(std::move(query)));
    s.ctes_.push_back(std::move(cte));
    return *this;
}
void db_query::with_columns(std::string_view name, const db_query& query, db_materialization materialization,
    std::span<const std::string> columns) {
    auto& s = storage();
    detail::validate_cte(s, name, columns);
    auto cte = detail::prepare_cte(s, name, false, materialization, columns);
    s.queries_.push_back(query.clone());
    s.ctes_.push_back(std::move(cte));
}
db_query& db_query::combine(db_set_operation operation, const db_query& query) {
    return combine(operation, query.clone());
}
db_query& db_query::combine(db_set_operation operation, db_query&& query) {
    auto& s = storage();
    if ((s.kind_ != db_query_kind::select && s.kind_ != db_query_kind::values) ||
        (query.storage().kind_ != db_query_kind::select && query.storage().kind_ != db_query_kind::values)) {
        throw std::invalid_argument("set operations require SELECT or VALUES queries");
    }
    const auto index = s.queries_.size();
    detail::reserve_append(s.queries_, 1);
    detail::reserve_append(s.set_operations_, 1);
    s.queries_.push_back(adopt(std::move(query)));
    s.set_operations_.push_back({operation, index});
    return *this;
}
db_statement& db_statement::operator=(db_statement&& other) {
    if (this != &other) {
        sql_ = std::move(other.sql_);
        params_ = std::move(other.params_);
        returns_rows_ = std::exchange(other.returns_rows_, false);
        other.sql_.clear();
        other.params_.clear();
    }
    return *this;
}

}  // namespace ruvia

namespace ruvia::detail {
namespace {

void validate_query_shape(const db_query_storage& s) {
    const bool select = s.kind_ == db_query_kind::select;
    const bool values = s.kind_ == db_query_kind::values;
    const bool insert = s.kind_ == db_query_kind::insert;
    const bool update = s.kind_ == db_query_kind::update;
    if (!select && (!s.projections_.empty() || s.distinct_ || !s.distinct_on_.empty() || !s.groups_.empty() || s.having_ != no_db_node)) {
        throw std::invalid_argument("SELECT clauses cannot be attached to a non-SELECT query");
    }
    if ((select || values) && (!s.returning_.empty() || !s.target_.empty())) {
        throw std::invalid_argument("a row query cannot have a DML target or RETURNING");
    }
    if ((insert || values) && (s.source_ || !s.joins_.empty() || s.predicate_ != no_db_node)) {
        throw std::invalid_argument("INSERT/VALUES cannot have FROM/JOIN/WHERE clauses");
    }
    if (!s.source_ && !s.joins_.empty()) {
        throw std::invalid_argument("JOIN requires a FROM source");
    }
    if (!insert && (s.conflict_ || s.insert_query_ != no_db_node || !s.columns_.empty())) {
        throw std::invalid_argument("INSERT clauses on a non-INSERT query");
    }
    if (!insert && !values && !s.rows_.empty()) {
        throw std::invalid_argument("VALUES on an incompatible query");
    }
    if (!update && !s.assignments_.empty()) {
        throw std::invalid_argument("SET on a non-UPDATE query");
    }
    if (!select && !values && (!s.orders_.empty() || s.limit_ || s.offset_ || s.lock_ || !s.set_operations_.empty())) {
        throw std::invalid_argument("ordering, pagination, locks and set operations require a row query");
    }
    if (s.lock_ && (!select || !s.set_operations_.empty() || s.distinct_ || !s.distinct_on_.empty() || !s.groups_.empty() || s.having_ != no_db_node)) {
        throw std::invalid_argument("row locks require an ungrouped SELECT without DISTINCT or set operations");
    }
}

bool is_mariadb_bare_function_name(std::string_view name) noexcept {
    if (name.empty() || name.size() > 64) {
        return false;
    }
    const auto is_ascii_letter = [](char value) noexcept {
        return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z');
    };
    if (!is_ascii_letter(name.front()) && name.front() != '_') {
        return false;
    }
    return std::ranges::all_of(name.substr(1), [&](char value) noexcept {
        return is_ascii_letter(value) || (value >= '0' && value <= '9') || value == '_' || value == '$';
    });
}

[[nodiscard]] bool same_db_value(const db_value& a, const db_value& b) noexcept {
    if (a.type() != b.type()) {
        return false;
    }
    switch (a.type()) {
        case db_value_type::null:
            return true;
        case db_value_type::string:
            return a.text() == b.text();
        case db_value_type::signed_value:
            return a.signed_value() == b.signed_value();
        case db_value_type::unsigned_value:
            return a.unsigned_value() == b.unsigned_value();
        case db_value_type::double_value:
            return a.double_value() == b.double_value();
        case db_value_type::bool_value:
            return a.bool_value() == b.bool_value();
    }
    return false;
}

struct date_part_sql_spec final {
    std::string_view token_;
    std::string_view pg_feature_{};
};

constexpr std::array<date_part_sql_spec, 11> date_part_sql_specs{{
    {"EPOCH", "EXTRACT EPOCH"},
    {"YEAR"},
    {"MONTH"},
    {"DAY"},
    {"HOUR"},
    {"MINUTE"},
    {"SECOND"},
    {"DOW", "EXTRACT DOW"},
    {"DOY", "EXTRACT DOY"},
    {"WEEK"},
    {"QUARTER"},
}};
static_assert(static_cast<std::size_t>(db_date_part::quarter) + 1 == date_part_sql_specs.size());

struct binary_operator_sql_spec final {
    std::string_view pg_token_;
    std::string_view maria_token_{};
    std::string_view pg_feature_{};
};

constexpr std::string_view pg_extended_operator_feature = "PostgreSQL JSON, array, regex or network operator";

constexpr std::array<binary_operator_sql_spec, 47> binary_operator_sql_specs{{
    {"="},
    {"<>"},
    {"<"},
    {"<="},
    {">"},
    {">="},
    {"AND"},
    {"OR"},
    {"+"},
    {"-"},
    {"*"},
    {"/"},
    {"%"},
    {"||"},
    {"LIKE"},
    {"NOT LIKE"},
    {"ILIKE", {}, "ILIKE"},
    {"NOT ILIKE", {}, "NOT ILIKE"},
    {"IN"},
    {"NOT IN"},
    {"IS DISTINCT FROM", "<=>"},
    {"IS NOT DISTINCT FROM", "<=>"},
    {"&"},
    {"|"},
    {"#", "^"},
    {"->", {}, pg_extended_operator_feature},
    {"->>", {}, pg_extended_operator_feature},
    {"#>", {}, pg_extended_operator_feature},
    {"#>>", {}, pg_extended_operator_feature},
    {"@>", {}, pg_extended_operator_feature},
    {"<@", {}, pg_extended_operator_feature},
    {"?", {}, pg_extended_operator_feature},
    {"?|", {}, pg_extended_operator_feature},
    {"?&", {}, pg_extended_operator_feature},
    {"||", {}, pg_extended_operator_feature},
    {"-", {}, pg_extended_operator_feature},
    {"#-", {}, pg_extended_operator_feature},
    {"@>", {}, pg_extended_operator_feature},
    {"<@", {}, pg_extended_operator_feature},
    {"&&", {}, pg_extended_operator_feature},
    {"~", {}, pg_extended_operator_feature},
    {"~*", {}, pg_extended_operator_feature},
    {">>", {}, pg_extended_operator_feature},
    {">>=", {}, pg_extended_operator_feature},
    {"<<", {}, pg_extended_operator_feature},
    {"<<=", {}, pg_extended_operator_feature},
    {"&&", {}, pg_extended_operator_feature},
}};
static_assert(static_cast<std::size_t>(db_binary_operator::inet_overlap) + 1 == binary_operator_sql_specs.size());

// How a bound PostgreSQL parameter is typed in compiled SQL. By default it is
// sent untyped and PostgreSQL infers its type from the surrounding SQL: the
// other operand, the target column, the parameter type of the selected
// function or operator overload, a Boolean context, an explicit cast. A value
// carries its C++ value category only where PostgreSQL provably cannot type an
// unknown parameter: "any" and polymorphic-only function arguments, numeric
// and boolean operands of arithmetic operators whose operands are all bound
// values, the null predicates, and COALESCE, NULLIF, GREATEST and LEAST over
// bound values only, which PostgreSQL would otherwise resolve as text.
enum class pg_parameter_cast : std::uint8_t {
    // The surrounding SQL types the value.
    context,
    // Nothing types the value: signed integers render ::int8, unsigned
    // integers ::numeric, doubles ::float8, booleans ::bool and text ::text.
    value_category,
    // Bare integer bitwise operands use bigint's signed range.
    bitwise_integer,
    // The PostgreSQL signature at this position accepts only int4; other
    // values stay untyped.
    int4,
};

struct pg_function_parameter_casts final {
    std::string_view name_;
    // Bit i selects argument i.
    std::uint32_t value_category_positions_;
    std::uint32_t int4_positions_;
};

constexpr std::uint32_t pg_every_argument = ~std::uint32_t{0};
// Every argument after a leading text one (a separator or format string).
constexpr std::uint32_t pg_after_first_argument = ~std::uint32_t{1};

// Built-in functions whose arguments at these positions are declared only as
// "any", anyelement or anycompatible, so an untyped argument cannot be
// resolved, and int4-only arguments, where an integer is typed int4 explicitly.
// Every other function argument keeps the type of its selected overload.
// Function names are emitted quoted, so the match is exact.
constexpr std::array pg_function_parameter_cast_table{
    pg_function_parameter_casts{"array_agg", pg_every_argument, 0},
    pg_function_parameter_casts{"array_length", 0, 0b10},
    pg_function_parameter_casts{"array_lower", 0, 0b10},
    pg_function_parameter_casts{"array_upper", 0, 0b10},
    pg_function_parameter_casts{"chr", 0, 0b1},
    pg_function_parameter_casts{"concat", pg_every_argument, 0},
    pg_function_parameter_casts{"concat_ws", pg_after_first_argument, 0},
    pg_function_parameter_casts{"format", pg_after_first_argument, 0},
    pg_function_parameter_casts{"json_agg", pg_every_argument, 0},
    pg_function_parameter_casts{"json_build_array", pg_every_argument, 0},
    pg_function_parameter_casts{"json_build_object", pg_every_argument, 0},
    pg_function_parameter_casts{"json_object_agg", pg_every_argument, 0},
    pg_function_parameter_casts{"jsonb_agg", pg_every_argument, 0},
    pg_function_parameter_casts{"jsonb_build_array", pg_every_argument, 0},
    pg_function_parameter_casts{"jsonb_build_object", pg_every_argument, 0},
    pg_function_parameter_casts{"jsonb_object_agg", pg_every_argument, 0},
    pg_function_parameter_casts{"lag", 0, 0b10},
    pg_function_parameter_casts{"lead", 0, 0b10},
    pg_function_parameter_casts{"left", 0, 0b10},
    pg_function_parameter_casts{"lpad", 0, 0b10},
    pg_function_parameter_casts{"make_date", 0, 0b111},
    pg_function_parameter_casts{"make_interval", 0, 0b111111},
    pg_function_parameter_casts{"make_time", 0, 0b11},
    pg_function_parameter_casts{"make_timestamp", 0, 0b11111},
    pg_function_parameter_casts{"make_timestamptz", 0, 0b11111},
    pg_function_parameter_casts{"nth_value", 0, 0b10},
    pg_function_parameter_casts{"ntile", 0, 0b1},
    pg_function_parameter_casts{"num_nonnulls", pg_every_argument, 0},
    pg_function_parameter_casts{"num_nulls", pg_every_argument, 0},
    pg_function_parameter_casts{"repeat", 0, 0b10},
    pg_function_parameter_casts{"right", 0, 0b10},
    pg_function_parameter_casts{"round", 0, 0b10},
    pg_function_parameter_casts{"rpad", 0, 0b10},
    pg_function_parameter_casts{"split_part", 0, 0b100},
    pg_function_parameter_casts{"substr", 0, 0b110},
    pg_function_parameter_casts{"substring", 0, 0b110},
    pg_function_parameter_casts{"to_json", pg_every_argument, 0},
    pg_function_parameter_casts{"to_jsonb", pg_every_argument, 0},
    pg_function_parameter_casts{"trim_array", 0, 0b10},
    pg_function_parameter_casts{"trunc", 0, 0b10},
    pg_function_parameter_casts{"width_bucket", 0, 0b1000},
};

static_assert(std::ranges::is_sorted(pg_function_parameter_cast_table, {}, &pg_function_parameter_casts::name_));

// The cast for argument `position` of `function`, unqualified or in pg_catalog.
[[nodiscard]] pg_parameter_cast pg_function_parameter_cast(std::string_view function, std::size_t position) noexcept {
    constexpr std::string_view catalog = "pg_catalog.";
    if (function.starts_with(catalog)) {
        function.remove_prefix(catalog.size());
    }
    if (position >= 32) {
        return pg_parameter_cast::context;
    }
    const auto found = std::ranges::lower_bound(
        pg_function_parameter_cast_table, function, {}, &pg_function_parameter_casts::name_);
    if (found != pg_function_parameter_cast_table.end() && found->name_ == function) {
        if ((found->int4_positions_ >> position) & 1U) {
            return pg_parameter_cast::int4;
        }
        if ((found->value_category_positions_ >> position) & 1U) {
            return pg_parameter_cast::value_category;
        }
    }
    return pg_parameter_cast::context;
}

// Two unknown arithmetic operands select no overload. Bitwise integer
// operands need an integral SQL type rather than the wider numeric category.
// Concatenation resolves unknown || unknown to text || text.
[[nodiscard]] pg_parameter_cast pg_binary_operand_cast(db_binary_operator value) noexcept {
    switch (value) {
        case db_binary_operator::add:
        case db_binary_operator::subtract:
        case db_binary_operator::multiply:
        case db_binary_operator::divide:
        case db_binary_operator::modulo:
            return pg_parameter_cast::value_category;
        case db_binary_operator::bit_and:
        case db_binary_operator::bit_or:
        case db_binary_operator::bit_xor:
            return pg_parameter_cast::bitwise_integer;
        default:
            return pg_parameter_cast::context;
    }
}

// A bound non-NULL value: it has no SQL type until a cast or its context gives one.
[[nodiscard]] bool is_untyped_value(const db_query_node& node) noexcept {
    return node.kind_ == db_node_kind::value && node.value_.type() != db_value_type::null;
}

// Only bare integers acquire the bitwise range contract. Other numeric and
// boolean values retain their category; strings retain SQL context because a
// reused parameter may already have been typed by an earlier reference.
[[nodiscard]] pg_parameter_cast pg_operator_operand_cast(const db_query_node& node, pg_parameter_cast cast) noexcept {
    if (!is_untyped_value(node) || node.value_.type() == db_value_type::string) {
        return pg_parameter_cast::context;
    }
    const auto type = node.value_.type();
    return cast == pg_parameter_cast::bitwise_integer &&
                   type != db_value_type::signed_value && type != db_value_type::unsigned_value
               ? pg_parameter_cast::value_category
               : cast;
}

// PostgreSQL's Bind message counts parameters in an Int16 (protocol 3.0).
constexpr std::size_t max_postgresql_parameters = 65535;

// PostgreSQL lexes `$` followed by digits as a positional parameter unless the
// `$` continues an identifier (`a$1`).
[[nodiscard]] bool contains_pg_positional_parameter(std::string_view text) noexcept {
    const auto is_digit = [](char value) noexcept { return value >= '0' && value <= '9'; };
    for (auto position = text.find('$'); position != std::string_view::npos; position = text.find('$', position + 1)) {
        if (position + 1 == text.size() || !is_digit(text[position + 1])) {
            continue;
        }
        if (position == 0) {
            return true;
        }
        const auto previous = static_cast<unsigned char>(text[position - 1]);
        const bool continues_identifier = previous >= 0x80 || previous == '_' || previous == '$' ||
                                          is_digit(static_cast<char>(previous)) ||
                                          (previous >= 'a' && previous <= 'z') || (previous >= 'A' && previous <= 'Z');
        if (!continues_identifier) {
            return true;
        }
    }
    return false;
}

class query_sql_writer final {
public:
    query_sql_writer(db_driver driver, db_parameter_mode mode)
        : driver_(driver),
          mode_(mode) {
        require_db_dialect(driver);
    }

    std::string sql_;
    std::vector<db_value> params_;
    [[nodiscard]] db_driver driver() const noexcept {
        return driver_;
    }

    bool pg() const noexcept {
        return driver_ == db_driver::postgresql;
    }

    [[noreturn]] static void unsupported(std::string_view feature) {
        throw std::invalid_argument(std::string(feature) + " is not supported by this database dialect");
    }

    void require_pg(std::string_view feature) const {
        if (!pg()) {
            unsupported(feature);
        }
    }

    void identifier(std::string_view name) {
        append_db_identifier(sql_, name, driver_);
    }

    void qualified(std::string_view name) {
        append_db_qualified_identifier(sql_, name, driver_);
    }

    void function_name(std::string_view name) {
        if (driver_ == db_driver::mariadb && is_mariadb_bare_function_name(name)) {
            sql_ += name;
        } else {
            qualified(name);
        }
    }

    // `cast` refines PostgreSQL parameter typing for the position the value
    // fills. Bare bitwise integers share the same range and SQL type in bound
    // and literal output; other casts leave literals to PostgreSQL's parser.
    void bound(const db_value& value, pg_parameter_cast cast = pg_parameter_cast::context) {
        append_parameter(value, nullptr, cast);
    }

    // Binds value node `index` of `s`. PostgreSQL numbers each node once,
    // however often grouped or repeated SQL emits it.
    void bound_node(const db_query_storage& s, std::size_t index, pg_parameter_cast cast) {
        append_parameter(s.nodes_[index].value_, parameter_position(s, index), cast);
    }

    void number(std::uint64_t value) {
        if (pg()) {
            if (!std::in_range<std::int64_t>(value)) {
                throw std::invalid_argument("PostgreSQL row count or offset exceeds bigint range");
            }
            bound(db_value(static_cast<std::int64_t>(value)));
        } else {
            bound(db_value(value));
        }
    }

    void type(const db_stored_type& value) {
        append_db_type_name(sql_, value.data_type_, value.custom_name_, value.length_, value.precision_,
            value.scale_, value.array_, driver_, db_sql_type_usage::cast);
    }

private:
    void append_parameter(const db_value& value, std::uint32_t* position, pg_parameter_cast cast) {
        const bool bitwise_integer = pg() && cast == pg_parameter_cast::bitwise_integer;
        if (bitwise_integer && value.type() == db_value_type::unsigned_value &&
            value.unsigned_value() > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())) {
            throw std::invalid_argument("PostgreSQL integer bitwise operand exceeds bigint range");
        }
        if (mode_ == db_parameter_mode::literal || value.type() == db_value_type::null) {
            if (bitwise_integer) {
                sql_ += '(';
            }
            append_db_literal(sql_, value, driver_);
            if (bitwise_integer) {
                // Cast the whole signed literal, including a possible minus.
                sql_ += ")::int8";
            }
            return;
        }
        if (position != nullptr && *position != 0) {
            pg_parameter(*position, value, cast);
            return;
        }
        if (pg() && params_.size() == max_postgresql_parameters) {
            throw std::length_error("PostgreSQL statement exceeds 65535 bound parameters");
        }
        params_.push_back(db_value(value));
        if (position != nullptr) {
            *position = static_cast<std::uint32_t>(params_.size());
        }
        if (pg()) {
            pg_parameter(params_.size(), value, cast);
        } else {
            sql_ += '?';
        }
    }

    // The parameter slot of a PostgreSQL bound value node, or nullptr where
    // parameters are not numbered.
    [[nodiscard]] std::uint32_t* parameter_position(const db_query_storage& s, std::size_t index) {
        if (!pg() || mode_ != db_parameter_mode::bound) {
            return nullptr;
        }
        if (positions_owner_ != &s) {
            const auto existing = std::ranges::find_if(parameter_positions_, [&s](const auto& entry_value) noexcept {
                return entry_value.owner_ == &s;
            });
            if (existing != parameter_positions_.end()) {
                positions_ = &existing->positions_;
            } else {
                auto& added = parameter_positions_.emplace_back(&s, std::vector<std::uint32_t>(s.nodes_.size(), 0));
                positions_ = &added.positions_;
            }
            positions_owner_ = &s;
        }
        return &(*positions_)[index];
    }

    void pg_parameter(std::size_t position, const db_value& value, pg_parameter_cast cast) {
        sql_ += '$';
        append_db_number(sql_, static_cast<std::uint64_t>(position));
        const auto type = value.type();
        const bool integer = type == db_value_type::signed_value || type == db_value_type::unsigned_value;
        if (cast == pg_parameter_cast::bitwise_integer) {
            sql_ += "::int8";
            return;
        }
        if (cast == pg_parameter_cast::int4) {
            if (integer) {
                sql_ += "::int4";
            }
            return;
        }
        if (cast == pg_parameter_cast::context) {
            return;
        }
        switch (type) {
            case db_value_type::signed_value:
                sql_ += "::int8";
                break;
            case db_value_type::unsigned_value:
                // The whole uint64 range exceeds bigint; numeric holds every
                // value exactly, independent of the bound value.
                sql_ += "::numeric";
                break;
            case db_value_type::double_value:
                sql_ += "::float8";
                break;
            case db_value_type::bool_value:
                sql_ += "::bool";
                break;
            case db_value_type::string:
                sql_ += "::text";
                break;
            case db_value_type::null:
                break;
        }
    }

    db_driver driver_;
    db_parameter_mode mode_;
    // One dense table per compiled storage, indexed by node and holding the
    // node's 1-based parameter number (0: not yet bound); positions_ caches
    // the table of positions_owner_.
    struct storage_parameter_positions final {
        const db_query_storage* owner_{nullptr};
        std::vector<std::uint32_t> positions_;
    };
    std::vector<storage_parameter_positions> parameter_positions_;
    const db_query_storage* positions_owner_{nullptr};
    std::vector<std::uint32_t>* positions_{nullptr};
};

}  // namespace

class db_query_compiler final {
public:
    db_query_compiler(db_driver driver, db_parameter_mode mode)
        : output_(driver, mode),
          mode_(mode),
          grouped_{nullptr, std::vector<std::pair<std::size_t, std::size_t>>{}} {}

    query_sql_writer output_;
    db_parameter_mode mode_;

    void expression(db_expression value) {
        if (value.empty()) {
            throw std::invalid_argument("empty database expression");
        }
        expr(*value.owner_, value.node_, 0);
    }
    void query(const db_query_storage& s, std::size_t depth = 0) {
        require_depth(depth);
        validate_query_shape(s);
        if (depth == 0) {
            output_.sql_.reserve(64U + s.nodes_.size() * 16U + s.projections_.size() * 12U +
                                 s.orders_.size() * 16U + s.joins_.size() * 32U + s.ctes_.size() * 48U);
        }
        auto enclosing_groups = std::exchange(grouped_, bound_groups(s));
        if (!s.ctes_.empty()) {
            if (!output_.pg() && s.kind_ != db_query_kind::select) {
                output_.unsupported("data-changing WITH");
            }
            output_.sql_ += "WITH ";
            if (std::ranges::any_of(s.ctes_, [](const auto& cte) { return cte.recursive_; })) {
                output_.sql_ += "RECURSIVE ";
            }
            separated(s.ctes_, [&](const auto& cte) {
                output_.identifier(cte.name_);
                if (!cte.columns_.empty()) {
                    output_.sql_ += " (";
                    identifiers(cte.columns_);
                    output_.sql_ += ')';
                }
                output_.sql_ += " AS ";
                switch (cte.materialization_) {
                    case db_materialization::default_value:
                        break;
                    case db_materialization::materialized:
                        output_.require_pg("materialized CTE");
                        output_.sql_ += "MATERIALIZED ";
                        break;
                    case db_materialization::not_materialized:
                        output_.require_pg("not-materialized CTE");
                        output_.sql_ += "NOT MATERIALIZED ";
                        break;
                }
                const auto& child_value = nested(s, cte.query_);
                if (child_value.kind_ != db_query_kind::select && child_value.kind_ != db_query_kind::values) {
                    output_.require_pg("data-changing CTE");
                    if (depth != 0) {
                        throw std::invalid_argument("data-changing CTE requires a top-level WITH statement");
                    }
                }
                output_.sql_ += '(';
                query(child_value, depth + 1);
                output_.sql_ += ')';
            });
            output_.sql_ += ' ';
        }
        for (std::size_t i = 0; i < s.set_operations_.size(); ++i) {
            output_.sql_ += '(';
        }
        body(s, depth + 1);
        for (const auto& operation : s.set_operations_) {
            output_.sql_ += ')';
            switch (operation.operation_) {
                case db_set_operation::union_value:
                    output_.sql_ += " UNION ";
                    break;
                case db_set_operation::union_all:
                    output_.sql_ += " UNION ALL ";
                    break;
                case db_set_operation::intersect:
                    output_.sql_ += " INTERSECT ";
                    break;
                case db_set_operation::intersect_all:
                    output_.require_pg("INTERSECT ALL");
                    output_.sql_ += " INTERSECT ALL ";
                    break;
                case db_set_operation::except:
                    output_.sql_ += " EXCEPT ";
                    break;
                case db_set_operation::except_all:
                    output_.require_pg("EXCEPT ALL");
                    output_.sql_ += " EXCEPT ALL ";
                    break;
            }
            output_.sql_ += '(';
            query(nested(s, operation.query_), depth + 1);
            output_.sql_ += ')';
        }
        if (!s.orders_.empty()) {
            output_.sql_ += " ORDER BY ";
            orders(s, s.orders_, depth + 1);
        }
        if (s.limit_) {
            output_.sql_ += " LIMIT ";
            output_.number(*s.limit_);
        } else if (s.offset_ && !output_.pg()) {
            output_.sql_ += " LIMIT 18446744073709551615";
        }
        if (s.offset_) {
            output_.sql_ += " OFFSET ";
            output_.number(*s.offset_);
        }
        if (s.lock_) {
            const auto& lock = *s.lock_;
            switch (lock.mode_) {
                case db_row_lock::update:
                    output_.sql_ += " FOR UPDATE";
                    break;
                case db_row_lock::no_key_update:
                    output_.require_pg("FOR NO KEY UPDATE");
                    output_.sql_ += " FOR NO KEY UPDATE";
                    break;
                case db_row_lock::share:
                    output_.sql_ += output_.pg() ? " FOR SHARE" : " LOCK IN SHARE MODE";
                    break;
                case db_row_lock::key_share:
                    output_.require_pg("FOR KEY SHARE");
                    output_.sql_ += " FOR KEY SHARE";
                    break;
            }
            if (!lock.tables_.empty()) {
                output_.require_pg("row lock OF");
                output_.sql_ += " OF ";
                identifiers(lock.tables_);
            }
            if (lock.nowait_) {
                output_.sql_ += " NOWAIT";
            }
            if (lock.skip_locked_) {
                if (!output_.pg() && lock.mode_ != db_row_lock::update) {
                    output_.unsupported("SKIP LOCKED for a shared lock");
                }
                output_.sql_ += " SKIP LOCKED";
            }
        }
        if (!s.returning_.empty()) {
            // MariaDB (>= 10.5) accepts RETURNING on INSERT and DELETE but not
            // on UPDATE (MDEV-10014).
            if (s.kind_ == db_query_kind::update) {
                output_.require_pg("UPDATE ... RETURNING");
            }
            output_.sql_ += " RETURNING ";
            expressions(s, s.returning_, depth + 1, true);
        }
        grouped_ = std::move(enclosing_groups);
    }

private:
    // Grouped bound expressions of one storage. PostgreSQL must see the same
    // parameter numbers wherever SELECT, GROUP BY and HAVING repeat a grouped
    // expression, but an imported copy has its own value nodes; each entry
    // maps such a GROUP BY expression to the node emitted for it.
    struct grouped_bound_values final {
        const db_query_storage* storage_{nullptr};
        // (GROUP BY node, node emitted for every equal expression)
        std::vector<std::pair<std::size_t, std::size_t>> groups_;
    };

    static void require_depth(std::size_t depth) {
        if (depth > max_db_query_depth) {
            throw std::length_error("database query nesting exceeds 256 levels");
        }
    }
    static const db_query_storage& nested(const db_query_storage& s, std::size_t index) {
        return s.queries_.at(index).storage();
    }
    [[nodiscard]] grouped_bound_values bound_groups(const db_query_storage& s) const {
        grouped_bound_values result{&s, std::vector<std::pair<std::size_t, std::size_t>>{}};
        if (!output_.pg() || mode_ != db_parameter_mode::bound) {
            return result;
        }
        for (const auto group : s.groups_) {
            if (!contains_bound_value(s, group)) {
                continue;
            }
            auto emitted = group;
            for (auto projection : s.projections_) {
                if (s.nodes_[projection].kind_ == db_node_kind::alias) {
                    projection = s.nodes_[projection].left_;
                }
                if (same_expression(s, group, projection)) {
                    emitted = projection;
                    break;
                }
            }
            result.groups_.emplace_back(group, emitted);
        }
        return result;
    }
    // The node to emit for `index`: the grouped expression it repeats, if any.
    [[nodiscard]] std::size_t grouped_node(const db_query_storage& s, std::size_t index) const {
        if (grouped_.storage_ != &s || grouped_.groups_.empty()) {
            return index;
        }
        const auto kind = s.nodes_.at(index).kind_;
        for (const auto& [group, emitted] : grouped_.groups_) {
            if (s.nodes_[group].kind_ == kind && same_expression(s, group, index)) {
                return emitted;
            }
        }
        return index;
    }

    template <class range_type, class fn_type>
    void separated(const range_type& range, fn_type&& fn) {
        bool first = true;
        for (const auto& item : range) {
            if (!first) {
                output_.sql_ += ", ";
            }
            first = false;
            fn(item);
        }
    }
    template <class range_type>
    void identifiers(const range_type& range) {
        separated(range, [&](const auto& name) { output_.identifier(name); });
    }
    template <class range_type>
    void expressions(const db_query_storage& s, const range_type& range, std::size_t depth, bool aliases = false) {
        separated(range, [&](std::size_t index) { expr(s, index, depth, aliases); });
    }
    void orders(const db_query_storage& s, const std::vector<db_stored_order>& terms, std::size_t depth) {
        separated(terms, [&](const auto& term) {
            if (!output_.pg() && term.nulls_ != db_nulls_order::default_value) {
                output_.sql_ += '(';
                expr(s, term.expression_, depth);
                output_.sql_ += " IS NULL)";
                output_.sql_ += term.nulls_ == db_nulls_order::first ? " DESC, " : " ASC, ";
            }
            expr(s, term.expression_, depth);
            output_.sql_ += term.direction_ == db_order_direction::asc ? " ASC" : " DESC";
            if (output_.pg()) {
                if (term.nulls_ == db_nulls_order::first) {
                    output_.sql_ += " NULLS FIRST";
                }
                if (term.nulls_ == db_nulls_order::last) {
                    output_.sql_ += " NULLS LAST";
                }
            }
        });
    }
    void boundary(const db_window_boundary& boundary) {
        if (boundary.kind_ != db_frame_boundary::preceding && boundary.kind_ != db_frame_boundary::following && boundary.offset_.value() != 0) {
            throw std::invalid_argument("a non-offset window frame boundary has an offset");
        }
        switch (boundary.kind_) {
            case db_frame_boundary::unbounded_preceding:
                output_.sql_ += "UNBOUNDED PRECEDING";
                break;
            case db_frame_boundary::preceding:
                append_db_number(output_.sql_, boundary.offset_.value());
                output_.sql_ += " PRECEDING";
                break;
            case db_frame_boundary::current_row:
                output_.sql_ += "CURRENT ROW";
                break;
            case db_frame_boundary::following:
                append_db_number(output_.sql_, boundary.offset_.value());
                output_.sql_ += " FOLLOWING";
                break;
            case db_frame_boundary::unbounded_following:
                output_.sql_ += "UNBOUNDED FOLLOWING";
                break;
        }
    }
    // Imports own their nodes. Equal grouped expressions must nevertheless use
    // the same PostgreSQL parameter nodes in SELECT, GROUP BY and HAVING.
    static bool same_expression(const db_query_storage& s, std::size_t lhs, std::size_t rhs, std::size_t depth = 0) {
        if (lhs == rhs) {
            return true;
        }
        if (lhs == no_db_node || rhs == no_db_node || depth > max_db_query_depth) {
            return false;
        }
        const auto& a = s.nodes_.at(lhs);
        const auto& b = s.nodes_.at(rhs);
        if (a.kind_ != b.kind_ || a.text_ != b.text_ || a.qualifier_ != b.qualifier_ || a.binary_ != b.binary_ ||
            a.unary_ != b.unary_ || a.date_part_ != b.date_part_ || a.flag_ != b.flag_ ||
            a.query_ != no_db_node || b.query_ != no_db_node || a.args_.size() != b.args_.size() ||
            a.named_.size() != b.named_.size() || a.orders_.size() != b.orders_.size() ||
            a.type_.data_type_ != b.type_.data_type_ || a.type_.custom_name_ != b.type_.custom_name_ ||
            a.type_.length_ != b.type_.length_ || a.type_.precision_ != b.type_.precision_ || a.type_.scale_ != b.type_.scale_ ||
            a.type_.array_ != b.type_.array_ || a.frame_.has_value() != b.frame_.has_value()) {
            return false;
        }
        if (a.frame_ && (a.frame_->kind_ != b.frame_->kind_ || a.frame_->start_.kind_ != b.frame_->start_.kind_ ||
                            a.frame_->start_.offset_ != b.frame_->start_.offset_ || a.frame_->end_.kind_ != b.frame_->end_.kind_ ||
                            a.frame_->end_.offset_ != b.frame_->end_.offset_)) {
            return false;
        }
        if (a.kind_ == db_node_kind::value && !same_db_value(a.value_, b.value_)) {
            return false;
        }
        const auto same = [&](std::size_t x, std::size_t y) { return same_expression(s, x, y, depth + 1); };
        if (!same(a.left_, b.left_) || !same(a.right_, b.right_)) {
            return false;
        }
        for (std::size_t i = 0; i < a.args_.size(); ++i) {
            if (!same(a.args_[i], b.args_[i])) {
                return false;
            }
        }
        for (std::size_t i = 0; i < a.named_.size(); ++i) {
            if (a.named_[i].name_ != b.named_[i].name_ || !same(a.named_[i].expression_, b.named_[i].expression_)) {
                return false;
            }
        }
        for (std::size_t i = 0; i < a.orders_.size(); ++i) {
            if (a.orders_[i].direction_ != b.orders_[i].direction_ || a.orders_[i].nulls_ != b.orders_[i].nulls_ ||
                !same(a.orders_[i].expression_, b.orders_[i].expression_)) {
                return false;
            }
        }
        return true;
    }
    [[nodiscard]] static bool contains_bound_value(
        const db_query_storage& s, std::size_t index, std::size_t depth = 0) noexcept {
        if (index >= s.nodes_.size() || depth > max_db_query_depth) {
            return false;
        }
        const auto& n = s.nodes_[index];
        switch (n.kind_) {
            case db_node_kind::column:
            case db_node_kind::star:
            case db_node_kind::default_value:
            case db_node_kind::excluded:
                return false;
            case db_node_kind::value:
                return n.value_.type() != db_value_type::null;
            default:
                break;
        }
        if (contains_bound_value(s, n.left_, depth + 1) ||
            contains_bound_value(s, n.right_, depth + 1)) {
            return true;
        }
        for (const auto arg : n.args_) {
            if (contains_bound_value(s, arg, depth + 1)) {
                return true;
            }
        }
        for (const auto& named : n.named_) {
            if (contains_bound_value(s, named.expression_, depth + 1)) {
                return true;
            }
        }
        for (const auto& order : n.orders_) {
            if (contains_bound_value(s, order.expression_, depth + 1)) {
                return true;
            }
        }
        return false;
    }
    // `cast` applies when the expression is a bound value; see pg_parameter_cast.
    void expr(const db_query_storage& s, std::size_t index, std::size_t depth, bool aliases = false,
        pg_parameter_cast cast = pg_parameter_cast::context) {
        require_depth(depth);
        index = grouped_node(s, index);
        const auto& n = s.nodes_.at(index);
        const auto child_value = [&](std::size_t id) { expr(s, id, depth + 1); };
        switch (n.kind_) {
            case db_node_kind::sql:
                if (output_.pg() && mode_ == db_parameter_mode::bound &&
                    (contains_pg_positional_parameter(n.text_) ||
                        std::ranges::any_of(n.named_, [](const auto& part) { return contains_pg_positional_parameter(part.name_); }))) {
                    // It would silently address the compiler's own numbering.
                    throw std::invalid_argument("raw SQL cannot contain PostgreSQL positional parameters; pass value() arguments");
                }
                output_.sql_ += '(';
                for (const auto& part : n.named_) {
                    output_.sql_ += part.name_;
                    // A following negative literal or `*` would otherwise
                    // start a `--` line comment or a `/*` block comment.
                    if (!part.name_.empty() && (part.name_.back() == '-' || part.name_.back() == '/')) {
                        output_.sql_ += ' ';
                    }
                    child_value(part.expression_);
                }
                output_.sql_ += n.text_;
                output_.sql_ += ')';
                break;
            case db_node_kind::column:
            case db_node_kind::star:
                if (!n.qualifier_.empty()) {
                    output_.qualified(n.qualifier_);
                    output_.sql_ += '.';
                }
                if (n.kind_ == db_node_kind::star) {
                    output_.sql_ += '*';
                } else {
                    output_.identifier(n.text_);
                }
                break;
            case db_node_kind::value:
                output_.bound_node(s, index, cast);
                break;
            case db_node_kind::default_value:
                output_.sql_ += "DEFAULT";
                break;
            case db_node_kind::excluded:
                if (output_.pg()) {
                    output_.sql_ += "excluded.";
                    output_.identifier(n.text_);
                } else {
                    output_.sql_ += "VALUES(";
                    output_.identifier(n.text_);
                    output_.sql_ += ')';
                }
                break;
            case db_node_kind::function:
            case db_node_kind::aggregate:
            case db_node_kind::coalesce:
            case db_node_kind::null_if:
            case db_node_kind::greatest:
            case db_node_kind::least: {
                switch (n.kind_) {
                    case db_node_kind::coalesce:
                        output_.sql_ += "COALESCE";
                        break;
                    case db_node_kind::null_if:
                        output_.sql_ += "NULLIF";
                        break;
                    case db_node_kind::greatest:
                        output_.sql_ += "GREATEST";
                        break;
                    case db_node_kind::least:
                        output_.sql_ += "LEAST";
                        break;
                    default:
                        output_.function_name(n.text_);
                        break;
                }
                output_.sql_ += '(';
                if (n.kind_ == db_node_kind::aggregate && n.flag_) {
                    output_.sql_ += "DISTINCT ";
                }
                // COALESCE, NULLIF, GREATEST and LEAST take their type from
                // their arguments; with only bound values there is none.
                const bool only_values = std::ranges::all_of(
                    n.args_, [&](std::size_t arg) { return s.nodes_.at(arg).kind_ == db_node_kind::value; });
                for (std::size_t position = 0; position < n.args_.size(); ++position) {
                    if (position != 0) {
                        output_.sql_ += ", ";
                    }
                    auto argument_cast = pg_parameter_cast::context;
                    if (output_.pg()) {
                        if (n.kind_ == db_node_kind::function || n.kind_ == db_node_kind::aggregate) {
                            argument_cast = pg_function_parameter_cast(n.text_, position);
                        } else if (only_values) {
                            argument_cast = pg_parameter_cast::value_category;
                        }
                    }
                    expr(s, n.args_[position], depth + 1, false, argument_cast);
                }
                if (!n.named_.empty()) {
                    output_.require_pg("named function arguments");
                    if (!n.args_.empty()) {
                        output_.sql_ += ", ";
                    }
                    separated(n.named_, [&](const auto& arg) { output_.identifier(arg.name_); output_.sql_ += " => "; child_value(arg.expression_); });
                }
                if (!n.orders_.empty()) {
                    output_.require_pg("ordered aggregate");
                    output_.sql_ += " ORDER BY ";
                    orders(s, n.orders_, depth + 1);
                }
                output_.sql_ += ')';
                break;
            }
            case db_node_kind::binary:
                binary(s, n, depth + 1);
                break;
            case db_node_kind::unary: {
                // Negation retains the value category; bitwise NOT selects an
                // integer type for bare integer operands. Null predicates take
                // any category. NOT and IS TRUE/FALSE coerce theirs to boolean.
                const auto& operand = s.nodes_.at(n.left_);
                auto operand_cast = pg_parameter_cast::context;
                if (output_.pg()) {
                    if (n.unary_ == db_unary_operator::negate || n.unary_ == db_unary_operator::bit_not) {
                        operand_cast = pg_operator_operand_cast(operand,
                            n.unary_ == db_unary_operator::bit_not ? pg_parameter_cast::bitwise_integer : pg_parameter_cast::value_category);
                    } else if ((n.unary_ == db_unary_operator::is_null ||
                                   n.unary_ == db_unary_operator::is_not_null) &&
                               is_untyped_value(operand)) {
                        operand_cast = pg_parameter_cast::value_category;
                    }
                }
                output_.sql_ += '(';
                switch (n.unary_) {
                    case db_unary_operator::not_value:
                        output_.sql_ += "NOT ";
                        child_value(n.left_);
                        break;
                    case db_unary_operator::negate:
                        // Keep the operator apart from its operand: a negative
                        // literal would otherwise render "--5", which
                        // PostgreSQL lexes as a line comment, and "~-5", which
                        // it lexes as the single operator "~-".
                        output_.sql_ += "- ";
                        expr(s, n.left_, depth + 1, false, operand_cast);
                        break;
                    case db_unary_operator::bit_not:
                        output_.sql_ += "~ ";
                        expr(s, n.left_, depth + 1, false, operand_cast);
                        break;
                    default:
                        expr(s, n.left_, depth + 1, false, operand_cast);
                        switch (n.unary_) {
                            case db_unary_operator::is_null:
                                output_.sql_ += " IS NULL";
                                break;
                            case db_unary_operator::is_not_null:
                                output_.sql_ += " IS NOT NULL";
                                break;
                            case db_unary_operator::is_true:
                                output_.sql_ += " IS TRUE";
                                break;
                            case db_unary_operator::is_false:
                                output_.sql_ += " IS FALSE";
                                break;
                            case db_unary_operator::is_not_true:
                                output_.sql_ += " IS NOT TRUE";
                                break;
                            case db_unary_operator::is_not_false:
                                output_.sql_ += " IS NOT FALSE";
                                break;
                            default:
                                std::terminate();
                        }
                }
                output_.sql_ += ')';
                break;
            }
            case db_node_kind::between:
                output_.sql_ += '(';
                child_value(n.args_[0]);
                output_.sql_ += n.flag_ ? " NOT BETWEEN " : " BETWEEN ";
                child_value(n.args_[1]);
                output_.sql_ += " AND ";
                child_value(n.args_[2]);
                output_.sql_ += ')';
                break;
            case db_node_kind::tuple:
            case db_node_kind::list:
            case db_node_kind::array:
                if (n.kind_ == db_node_kind::array) {
                    output_.require_pg("SQL arrays");
                    output_.sql_ += "ARRAY[";
                } else {
                    if (n.args_.empty()) {
                        throw std::invalid_argument("empty SQL tuple or list");
                    }
                    output_.sql_ += n.kind_ == db_node_kind::tuple ? "ROW(" : "(";
                }
                expressions(s, n.args_, depth + 1);
                output_.sql_ += n.kind_ == db_node_kind::array ? ']' : ')';
                break;
            case db_node_kind::any:
            case db_node_kind::all:
                output_.require_pg("array ANY/ALL");
                output_.sql_ += n.kind_ == db_node_kind::any ? "ANY(" : "ALL(";
                child_value(n.left_);
                output_.sql_ += ')';
                break;
            case db_node_kind::cast:
                output_.sql_ += "CAST(";
                child_value(n.left_);
                output_.sql_ += " AS ";
                output_.type(n.type_);
                output_.sql_ += ')';
                break;
            case db_node_kind::alias:
                if (!aliases) {
                    throw std::invalid_argument("an alias is only valid in a projection or RETURNING");
                }
                child_value(n.left_);
                output_.sql_ += " AS ";
                output_.identifier(n.text_);
                break;
            case db_node_kind::case_value:
                output_.sql_ += "CASE";
                for (std::size_t i = 0; i < n.args_.size(); i += 2) {
                    output_.sql_ += " WHEN ";
                    child_value(n.args_[i]);
                    output_.sql_ += " THEN ";
                    child_value(n.args_[i + 1]);
                }
                if (n.left_ != no_db_node) {
                    output_.sql_ += " ELSE ";
                    child_value(n.left_);
                }
                output_.sql_ += " END";
                break;
            case db_node_kind::exists:
            case db_node_kind::subquery:
                if (n.kind_ == db_node_kind::exists) {
                    output_.sql_ += "EXISTS ";
                }
                output_.sql_ += '(';
                query(nested(s, n.query_), depth + 1);
                output_.sql_ += ')';
                break;
            case db_node_kind::filter:
                output_.require_pg("aggregate FILTER");
                child_value(n.left_);
                output_.sql_ += " FILTER (WHERE ";
                child_value(n.right_);
                output_.sql_ += ')';
                break;
            case db_node_kind::window: {
                child_value(n.left_);
                output_.sql_ += " OVER (";
                bool space = false;
                if (!n.args_.empty()) {
                    output_.sql_ += "PARTITION BY ";
                    expressions(s, n.args_, depth + 1);
                    space = true;
                }
                if (!n.orders_.empty()) {
                    if (space) {
                        output_.sql_ += ' ';
                    }
                    output_.sql_ += "ORDER BY ";
                    orders(s, n.orders_, depth + 1);
                    space = true;
                }
                if (n.frame_) {
                    if (space) {
                        output_.sql_ += ' ';
                    }
                    const auto& frame = *n.frame_;
                    if (frame.start_.kind_ == db_frame_boundary::unbounded_following || frame.end_.kind_ == db_frame_boundary::unbounded_preceding || frame.start_.kind_ > frame.end_.kind_) {
                        throw std::invalid_argument("invalid window frame boundaries");
                    }
                    switch (frame.kind_) {
                        case db_window_frame::rows:
                            output_.sql_ += "ROWS";
                            break;
                        case db_window_frame::range:
                            output_.sql_ += "RANGE";
                            break;
                        case db_window_frame::groups:
                            output_.require_pg("GROUPS window frame");
                            output_.sql_ += "GROUPS";
                            break;
                    }
                    output_.sql_ += " BETWEEN ";
                    boundary(frame.start_);
                    output_.sql_ += " AND ";
                    boundary(frame.end_);
                }
                output_.sql_ += ')';
                break;
            }
            case db_node_kind::within_group:
                output_.require_pg("ordered-set aggregate");
                child_value(n.left_);
                output_.sql_ += " WITHIN GROUP (ORDER BY ";
                orders(s, n.orders_, depth + 1);
                output_.sql_ += ')';
                break;
            case db_node_kind::extract: {
                const auto& spec = date_part_sql_specs.at(static_cast<std::size_t>(n.date_part_));
                if (!spec.pg_feature_.empty()) {
                    output_.require_pg(spec.pg_feature_);
                }
                output_.sql_ += "EXTRACT(";
                output_.sql_ += spec.token_;
                output_.sql_ += " FROM ";
                child_value(n.left_);
                output_.sql_ += ')';
                break;
            }
            case db_node_kind::subscript:
                output_.require_pg("array subscript");
                output_.sql_ += '(';
                child_value(n.left_);
                output_.sql_ += ")[";
                child_value(n.right_);
                output_.sql_ += ']';
                break;
            case db_node_kind::collate:
                output_.sql_ += '(';
                child_value(n.left_);
                output_.sql_ += " COLLATE ";
                output_.qualified(n.text_);
                output_.sql_ += ')';
                break;
            case db_node_kind::at_time_zone: {
                output_.require_pg("AT TIME ZONE");
                const auto& zone = s.nodes_.at(n.right_);
                const auto zone_cast = is_untyped_value(zone) ? pg_parameter_cast::value_category : pg_parameter_cast::context;
                output_.sql_ += '(';
                child_value(n.left_);
                output_.sql_ += " AT TIME ZONE ";
                expr(s, n.right_, depth + 1, false, zone_cast);
                output_.sql_ += ')';
                break;
            }
        }
    }
    // Emits a left-deep AND/OR chain flat, `(a AND b AND c)`, at one nesting
    // level. A link that repeats a grouped expression goes through expr().
    void logical_chain(const db_query_storage& s, const db_query_node& n, std::size_t depth) {
        std::vector<const db_query_node*> spine;
        spine.push_back(&n);
        while (continues_logical_chain(s.nodes_, *spine.back(), spine.back()->left_) &&
               grouped_node(s, spine.back()->left_) == spine.back()->left_) {
            spine.push_back(&s.nodes_[spine.back()->left_]);
        }
        const std::string_view token = n.binary_ == db_binary_operator::and_value ? " AND " : " OR ";
        output_.sql_ += '(';
        expr(s, spine.back()->left_, depth);
        for (auto link = spine.rbegin(); link != spine.rend(); ++link) {
            output_.sql_ += token;
            expr(s, (*link)->right_, depth);
        }
        output_.sql_ += ')';
    }
    void binary(const db_query_storage& s, const db_query_node& n, std::size_t depth) {
        if (continues_logical_chain(s.nodes_, n, n.left_) && grouped_node(s, n.left_) == n.left_) {
            logical_chain(s, n, depth);
            return;
        }
        const auto& left = s.nodes_.at(n.left_);
        const auto& right = s.nodes_.at(n.right_);
        if ((n.binary_ == db_binary_operator::in || n.binary_ == db_binary_operator::not_in) && right.kind_ == db_node_kind::list && right.args_.empty()) {
            output_.sql_ += n.binary_ == db_binary_operator::in ? "FALSE" : "TRUE";
            return;
        }
        if (n.binary_ == db_binary_operator::concat && !output_.pg()) {
            output_.sql_ += "CONCAT(";
            expr(s, n.left_, depth);
            output_.sql_ += ", ";
            expr(s, n.right_, depth);
            output_.sql_ += ')';
            return;
        }
        const bool maria_distinct = !output_.pg() && n.binary_ == db_binary_operator::is_distinct_from;
        output_.sql_ += '(';
        if (maria_distinct) {
            output_.sql_ += "NOT (";
        }
        auto left_cast = pg_parameter_cast::context;
        auto right_cast = pg_parameter_cast::context;
        if (output_.pg()) {
            // Preserve the other operand's SQL type. Only arithmetic over two
            // bare values needs an explicit choice of overload.
            const auto operand_cast = pg_binary_operand_cast(n.binary_);
            if (operand_cast != pg_parameter_cast::context && is_untyped_value(left) && is_untyped_value(right)) {
                left_cast = pg_operator_operand_cast(left, operand_cast);
                right_cast = pg_operator_operand_cast(right, operand_cast);
            }
            // jsonb -> n, ->> n and - n index arrays with int4 only; untyped,
            // an integer would be read as a text key.
            if (n.binary_ == db_binary_operator::json_get || n.binary_ == db_binary_operator::json_get_text ||
                n.binary_ == db_binary_operator::json_delete) {
                right_cast = pg_parameter_cast::int4;
            }
        }
        expr(s, n.left_, depth, false, left_cast);
        const auto op_index = static_cast<std::size_t>(n.binary_);
        if (op_index >= binary_operator_sql_specs.size()) {
            std::terminate();
        }
        const auto& op_spec = binary_operator_sql_specs[op_index];
        if (!op_spec.pg_feature_.empty()) {
            output_.require_pg(op_spec.pg_feature_);
        }
        const auto token = (!output_.pg() && !op_spec.maria_token_.empty()) ? op_spec.maria_token_ : op_spec.pg_token_;
        if ((n.binary_ == db_binary_operator::in || n.binary_ == db_binary_operator::not_in) && right.kind_ != db_node_kind::list && right.kind_ != db_node_kind::subquery) {
            throw std::invalid_argument("IN requires a list or a subquery");
        }
        output_.sql_ += ' ';
        output_.sql_ += token;
        output_.sql_ += ' ';
        expr(s, n.right_, depth, false, right_cast);
        if (maria_distinct) {
            output_.sql_ += ')';
        }
        output_.sql_ += ')';
    }
    void source(const db_query_storage& s, const db_query_source& value, std::size_t depth) {
        if (value.lateral_) {
            output_.require_pg("LATERAL");
            output_.sql_ += "LATERAL ";
        }
        switch (value.kind_) {
            case db_source_kind::table:
                output_.qualified(value.name_);
                break;
            case db_source_kind::query:
                output_.sql_ += '(';
                query(nested(s, value.query_), depth + 1);
                output_.sql_ += ')';
                break;
            case db_source_kind::function:
                output_.require_pg("table function");
                expr(s, value.expression_, depth + 1);
                break;
        }
        if (value.ordinality_) {
            output_.require_pg("WITH ORDINALITY");
            if (value.kind_ != db_source_kind::function) {
                throw std::invalid_argument("WITH ORDINALITY requires a table function");
            }
            output_.sql_ += " WITH ORDINALITY";
        }
        if (!value.alias_.empty()) {
            output_.sql_ += " AS ";
            output_.identifier(value.alias_);
        }
        if (!value.columns_.empty()) {
            if (value.alias_.empty()) {
                throw std::invalid_argument("source column names require an alias");
            }
            output_.sql_ += " (";
            const bool typed = value.columns_.front().type_.data_type_ != db_data_type::inferred || !value.columns_.front().type_.custom_name_.empty();
            if (typed && (value.kind_ != db_source_kind::function || value.ordinality_)) {
                throw std::invalid_argument("record definitions require a function without ordinality");
            }
            separated(value.columns_, [&](const auto& column) {
                const bool has_type = column.type_.data_type_ != db_data_type::inferred || !column.type_.custom_name_.empty();
                if (has_type != typed) {
                    throw std::invalid_argument("source column definitions must all specify a type or all omit it");
                }
                output_.identifier(column.name_);
                if (typed) {
                    output_.sql_ += ' ';
                    const auto& t = column.type_;
                    append_db_type_name(output_.sql_, t.data_type_, t.custom_name_, t.length_, t.precision_, t.scale_, t.array_, output_.driver());
                }
            });
            output_.sql_ += ')';
        }
    }
    void sources(const db_query_storage& s, std::size_t depth) {
        source(s, *s.source_, depth);
        for (const auto& join : s.joins_) {
            switch (join.type_) {
                case db_join_type::inner:
                    output_.sql_ += " INNER JOIN ";
                    break;
                case db_join_type::left:
                    output_.sql_ += " LEFT JOIN ";
                    break;
                case db_join_type::right:
                    output_.sql_ += " RIGHT JOIN ";
                    break;
                case db_join_type::full:
                    output_.require_pg("FULL JOIN");
                    output_.sql_ += " FULL JOIN ";
                    break;
                case db_join_type::cross:
                    output_.sql_ += " CROSS JOIN ";
                    break;
            }
            if (join.type_ == db_join_type::cross) {
                if (join.on_ != no_db_node || !join.using_columns_.empty()) {
                    throw std::invalid_argument("CROSS JOIN has no ON or USING");
                }
            } else if ((join.on_ != no_db_node) == !join.using_columns_.empty()) {
                throw std::invalid_argument("JOIN requires exactly one of ON or USING");
            }
            source(s, join.source_, depth + 1);
            if (join.on_ != no_db_node) {
                output_.sql_ += " ON ";
                expr(s, join.on_, depth + 1);
            }
            if (!join.using_columns_.empty()) {
                output_.sql_ += " USING (";
                identifiers(join.using_columns_);
                output_.sql_ += ')';
            }
        }
    }
    void assignments(const db_query_storage& s, const std::vector<db_stored_assignment>& values, std::size_t depth) {
        if (values.empty()) {
            throw std::invalid_argument("UPDATE requires assignments");
        }
        separated(values, [&](const auto& assignment) { output_.identifier(assignment.column_); output_.sql_ += " = "; expr(s, assignment.expression_, depth + 1); });
    }
    void conflict(const db_query_storage& s, std::size_t depth) {
        const auto& value = *s.conflict_;
        if (value.do_nothing_ && (!value.assignments_.empty() || value.update_where_ != no_db_node)) {
            throw std::invalid_argument("DO NOTHING cannot specify updates");
        }
        if (output_.pg()) {
            if (value.any_unique_key_) {
                throw std::invalid_argument("any_unique_key is specific to MariaDB");
            }
            if (!value.columns_.empty() && !value.constraint_.empty()) {
                throw std::invalid_argument("conflict target cannot have both columns and a constraint");
            }
            if (value.target_where_ != no_db_node && value.columns_.empty()) {
                throw std::invalid_argument("a conflict predicate requires target columns");
            }
            output_.sql_ += " ON CONFLICT";
            if (!value.columns_.empty()) {
                output_.sql_ += " (";
                identifiers(value.columns_);
                output_.sql_ += ')';
            }
            if (!value.constraint_.empty()) {
                output_.sql_ += " ON CONSTRAINT ";
                output_.identifier(value.constraint_);
            }
            if (value.target_where_ != no_db_node) {
                output_.sql_ += " WHERE ";
                expr(s, value.target_where_, depth + 1);
            }
            if (value.do_nothing_) {
                output_.sql_ += " DO NOTHING";
                return;
            }
            if (value.columns_.empty() && value.constraint_.empty()) {
                throw std::invalid_argument("DO UPDATE requires a conflict target");
            }
            output_.sql_ += " DO UPDATE SET ";
            assignments(s, value.assignments_, depth);
            if (value.update_where_ != no_db_node) {
                output_.sql_ += " WHERE ";
                expr(s, value.update_where_, depth + 1);
            }
        } else {
            if (!value.any_unique_key_ || !value.columns_.empty() || !value.constraint_.empty() || value.target_where_ != no_db_node || value.update_where_ != no_db_node || value.do_nothing_) {
                output_.unsupported("targeted or conditional upsert / DO NOTHING");
            }
            output_.sql_ += " ON DUPLICATE KEY UPDATE ";
            assignments(s, value.assignments_, depth);
        }
    }
    void rows(const db_query_storage& s, std::size_t depth) {
        if (s.rows_.empty() || s.rows_.front().empty()) {
            throw std::invalid_argument("VALUES requires nonempty rows");
        }
        const auto width = s.rows_.front().size();
        if (!s.columns_.empty() && width != s.columns_.size()) {
            throw std::invalid_argument("INSERT column and row widths differ");
        }
        output_.sql_ += "VALUES ";
        separated(s.rows_, [&](const auto& row) {
            if (row.size() != width) {
                throw std::invalid_argument("VALUES rows have different widths");
            }
            output_.sql_ += '(';
            expressions(s, row, depth + 1);
            output_.sql_ += ')';
        });
    }
    void body(const db_query_storage& s, std::size_t depth) {
        switch (s.kind_) {
            case db_query_kind::select:
                output_.sql_ += "SELECT ";
                if (s.distinct_) {
                    output_.sql_ += "DISTINCT ";
                }
                if (!s.distinct_on_.empty()) {
                    output_.require_pg("DISTINCT ON");
                    output_.sql_ += "DISTINCT ON (";
                    expressions(s, s.distinct_on_, depth + 1);
                    output_.sql_ += ") ";
                }
                if (s.projections_.empty()) {
                    output_.sql_ += '*';
                } else {
                    expressions(s, s.projections_, depth + 1, true);
                }
                if (s.source_) {
                    output_.sql_ += " FROM ";
                    sources(s, depth + 1);
                }
                break;
            case db_query_kind::values:
                rows(s, depth);
                break;
            case db_query_kind::insert:
                output_.sql_ += "INSERT INTO ";
                output_.qualified(s.target_);
                if (!s.target_alias_.empty()) {
                    output_.require_pg("INSERT target alias");
                    output_.sql_ += " AS ";
                    output_.identifier(s.target_alias_);
                }
                if (!s.columns_.empty()) {
                    output_.sql_ += " (";
                    identifiers(s.columns_);
                    output_.sql_ += ')';
                }
                output_.sql_ += ' ';
                if (s.insert_query_ != no_db_node) {
                    if (!s.rows_.empty()) {
                        throw std::invalid_argument("INSERT cannot have both VALUES and SELECT");
                    }
                    query(nested(s, s.insert_query_), depth + 1);
                } else if (s.rows_.empty() ||
                           (s.columns_.empty() && s.rows_.size() == 1 && s.rows_.front().empty())) {
                    if (!s.columns_.empty()) {
                        throw std::invalid_argument("INSERT column list requires input rows");
                    }
                    output_.sql_ += output_.pg() ? "DEFAULT VALUES" : "() VALUES ()";
                } else {
                    rows(s, depth);
                }
                if (s.conflict_) {
                    conflict(s, depth + 1);
                }
                break;
            case db_query_kind::update:
            case db_query_kind::delete_value:
                output_.sql_ += s.kind_ == db_query_kind::update ? "UPDATE " : "DELETE FROM ";
                output_.qualified(s.target_);
                if (!s.target_alias_.empty()) {
                    if (!output_.pg() && s.kind_ == db_query_kind::delete_value) {
                        output_.unsupported("DELETE target alias");
                    }
                    output_.sql_ += " AS ";
                    output_.identifier(s.target_alias_);
                }
                if (s.kind_ == db_query_kind::update) {
                    output_.sql_ += " SET ";
                    assignments(s, s.assignments_, depth + 1);
                }
                if (s.source_) {
                    output_.require_pg("UPDATE FROM / DELETE USING");
                    output_.sql_ += s.kind_ == db_query_kind::update ? " FROM " : " USING ";
                    sources(s, depth + 1);
                }
                break;
        }
        if (s.predicate_ != no_db_node) {
            output_.sql_ += " WHERE ";
            expr(s, s.predicate_, depth + 1);
        }
        if (!s.groups_.empty()) {
            output_.sql_ += " GROUP BY ";
            expressions(s, s.groups_, depth + 1);
        }
        if (s.having_ != no_db_node) {
            output_.sql_ += " HAVING ";
            expr(s, s.having_, depth + 1);
        }
    }

    grouped_bound_values grouped_;
};
void append_db_expression(std::string& output, db_expression expression, db_driver driver) {
    if (expression.empty()) {
        throw std::invalid_argument("empty database expression");
    }
    db_query_compiler compiler(driver, db_parameter_mode::literal);
    const auto initial_size = output.size();
    compiler.output_.sql_ = std::move(output);
    try {
        if (compiler.output_.sql_.capacity() - initial_size < 32U) {
            compiler.output_.sql_.reserve(initial_size + 64U);
        }
        compiler.expression(expression);
        output = std::move(compiler.output_.sql_);
    } catch (...) {
        compiler.output_.sql_.resize(initial_size);
        output = std::move(compiler.output_.sql_);
        throw;
    }
}

}  // namespace ruvia::detail

namespace ruvia {

db_statement db_query::compile(db_driver driver, db_parameter_mode mode) const {
    detail::db_query_compiler compiler(driver, mode);
    compiler.query(storage());
    return db_statement(std::move(compiler.output_.sql_), std::move(compiler.output_.params_), returns_rows());
}

std::string db_query::render_expression(db_expression expression, db_driver driver) {
    std::string output;
    detail::append_db_expression(output, expression, driver);
    return output;
}

}  // namespace ruvia
