#include <array>
#include <chrono>
#include <concepts>
#include <coroutine>
#include <cstdint>
#include <future>
#include <initializer_list>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

#include <asio/co_spawn.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/post.hpp>
#include <asio/read.hpp>
#include <asio/use_future.hpp>
#include <asio/write.hpp>

#include "ruvia/core/asio_task.h"
#include "ruvia/core/event_loop_attachment.h"
#include "ruvia/web/db/db.h"
#include "ruvia/web/db/db_expressions.h"
#include "ruvia/web/detail/db/db_operation_state.h"
#include "ruvia/web/detail/db/db_result_access.h"
#include "ruvia/web/detail/db/db_value_access.h"

#include "db/db_config_validation.h"
#include "db/db_pool_operations.h"
#include "db/db_prepared_statement.h"
#include "db/db_registry.h"
#include "db/db_slot_socket.h"
#include "memory_resource_fixture.h"
#include "test_harness.h"
#include "test_io_context.h"
#ifdef RUVIA_ENABLE_MARIADB
#include "db/db_mysql_runtime.h"
#endif

namespace {

using ruvia::test::rejecting_memory_resource;
using ruvia::test::tracking_resource;
using ruvia::testing::throws_on;

std::size_t owned_string_allocations() {
    ruvia::test::counting_memory_resource memory;
    // Include implementation-owned storage such as MSVC debug iterator proxies.
    const std::pmr::string input(256, 'x', &memory);
    return memory.live_allocations();
}

[[nodiscard]] ruvia::db_config test_db_config() {
#ifdef RUVIA_ENABLE_MARIADB
    return ruvia::db_config{.driver_ = ruvia::db_driver::mariadb};
#else
    return ruvia::db_config{.driver_ = ruvia::db_driver::postgresql};
#endif
}

#if defined(RUVIA_ENABLE_MARIADB) || defined(RUVIA_ENABLE_POSTGRESQL)
struct closing_resolve_slot;

class closing_resolver final {
public:
    explicit closing_resolver(closing_resolve_slot& slot) noexcept
        : slot_(&slot) {}

    template <typename handler_type>
    void async_resolve(std::string_view, std::string_view, handler_type handler);

    void cancel() noexcept {}

private:
    closing_resolve_slot* slot_;
};

struct closing_resolve_slot final {
    enum class deadline_kind_type : std::uint8_t { resolve };

    closing_resolve_slot()
        : resolver_(*this) {}

    bool wait_active_{false};
    bool close_requested_{false};
    bool observed_active_resolve_{false};
    closing_resolver resolver_;
    bool throw_on_initiation_{false};
    ruvia::worker_timer_registration timer_;
    ruvia::worker_timer_registration* deadline_timer_{&timer_};
    ruvia::operation_deadline<deadline_kind_type> deadline_;

    static void expire_deadline(closing_resolve_slot& slot, deadline_kind_type) noexcept {
        slot.resolver_.cancel();
    }
};

template <typename handler_type>
void closing_resolver::async_resolve(std::string_view, std::string_view, handler_type handler) {
    slot_->observed_active_resolve_ = slot_->wait_active_;
    if (slot_->throw_on_initiation_) {
        throw std::runtime_error("resolver initiation failed");
    }
    slot_->close_requested_ = true;
    handler(asio::error::operation_aborted, asio::ip::tcp::resolver::results_type{});
}

struct closing_resolve_pool final {
    struct config_type final {
        std::string host_{"resolver.test"};
        std::uint16_t port_{3306};
        ruvia::db_driver driver_{ruvia::db_driver::mariadb};
    } config_;

    std::pmr::memory_resource* resource_{std::pmr::get_default_resource()};
    ruvia::worker_handle worker_;

    void throw_if_cancelled(const closing_resolve_slot&) const {}
};
#endif

[[nodiscard]] ruvia::detail::db_definition db_definition(std::string_view alias,
    const ruvia::db_config& config,
    std::pmr::memory_resource* resource = std::pmr::get_default_resource()) {
    return {
        std::pmr::string(alias, resource),
        ruvia::detail::db_config_storage(config, resource),
    };
}

using guarded_lease_type = std::pmr::string;
using guarded_lease_state_type = ruvia::detail::db_operation_state<guarded_lease_type>;
using guarded_lease_guard_type = ruvia::detail::db_operation_guard<guarded_lease_type>;

struct guarded_lease_gate final {
    [[nodiscard]] bool await_ready() const noexcept {
        return false;
    }
    void await_suspend(std::coroutine_handle<> continuation) noexcept {
        continuation_ = continuation;
    }
    void await_resume() const noexcept {}
    void resume() noexcept {
        auto continuation = std::exchange(continuation_, {});
        continuation.resume();
    }
    std::coroutine_handle<> continuation_{};
};

class guarded_lease_capability final {
public:
    guarded_lease_capability(ruvia::operation_scope& scope, guarded_lease_state_type& state_value,
        bool& expired) noexcept
        : state_(state_value),
          expired_(expired),
          registration_(scope, this, &guarded_lease_capability::expire) {}

private:
    static void expire(void* target) noexcept {
        auto& capability = *static_cast<guarded_lease_capability*>(target);
        capability.state_.reset([](guarded_lease_type&) noexcept {});
        capability.expired_ = true;
    }

    guarded_lease_state_type& state_;
    bool& expired_;
    ruvia::scoped_capability_registration registration_;
};

ruvia::task<void> fail_guarded_lease_after_gate(
    guarded_lease_guard_type pending, guarded_lease_gate& gate_value, std::pmr::string input) {
    guarded_lease_guard_type operation(std::move(pending));
    operation.start();
    co_await gate_value;
    (void)input;
    throw std::runtime_error("scoped database operation failed");
}

ruvia::task<void> complete_guarded_lease(guarded_lease_guard_type pending, std::pmr::string input) {
    guarded_lease_guard_type operation(std::move(pending));
    operation.start();
    (void)input;
    operation.finish_active();
    co_return;
}

ruvia::task<void> await_scoped_operation(ruvia::scoped_operation<void>& operation) {
    co_await std::move(operation);
    co_return;
}

ruvia::task<void> join_scoped_operations(ruvia::operation_scope& scope) {
    co_await scope.close_and_join();
    co_return;
}

class db_registry_test_runtime final {
#ifndef _WIN32
    asio::io_context owned_io_context_;
#endif

public:
#ifdef _WIN32
    db_registry_test_runtime()
        : io_context_(ruvia::test::new_test_io_context()),
          attachment_(ruvia::attach_event_loop(io_context_)),
          worker_(attachment_.loop().handle()) {}
#else
    db_registry_test_runtime()
        : io_context_(owned_io_context_),
          attachment_(ruvia::attach_event_loop(io_context_)),
          worker_(attachment_.loop().handle()) {}
#endif

    asio::io_context& io_context_;
    ruvia::event_loop_attachment attachment_;
    ruvia::worker_handle worker_;
};

struct deadline_test_slot final {
    enum class deadline_kind : std::uint8_t { resolve,
        socket,
        sleep };

    static void expire_deadline(deadline_test_slot& slot, deadline_kind kind) noexcept {
        ++slot.expiry_count_;
        slot.last_expired_ = kind;
        auto continuation = std::exchange(slot.deadline_continuation_, {});
        if (continuation) {
            continuation.resume();
        }
    }

    ruvia::worker_timer_registration timer_;
    ruvia::worker_timer_registration* deadline_timer_{&timer_};
    ruvia::operation_deadline<deadline_kind> deadline_;
    std::coroutine_handle<> deadline_continuation_{};
    unsigned expiry_count_{0};
    std::optional<deadline_kind> last_expired_;
};

// Bound parameters passed as ordinary arguments.

// A prepared sequence must keep selecting the span overload rather than being
// absorbed as a single bound parameter, which would send the wrong argument.

// Variadic calls clone an owning-string temporary before returning, while the
// storable db_value type above continues to reject the same temporary.

// An lvalue string is fine: it outlives the call, which is all the synchronous
// parameter cloning requires.

}  // namespace

RUVIA_TEST(db_slot_deadline_replacement_cancel_and_disable_retire_previous_action) {
    using namespace std::chrono_literals;
    using kind = deadline_test_slot::deadline_kind;
    db_registry_test_runtime runtime;
    deadline_test_slot slot;
    asio::post(runtime.io_context_, [&] {
        ruvia::detail::arm_db_slot_deadline(runtime.worker_, slot, 1h, kind::resolve);
        slot.deadline_continuation_ = std::noop_coroutine();
        ruvia::detail::arm_db_slot_deadline(runtime.worker_, slot, 1ms, kind::socket);
        RUVIA_CHECK(!slot.deadline_continuation_);
    });
    // The attachment retains its owner loop; wait for expiry, not loop exit.
    while (slot.expiry_count_ != 1) {
        runtime.io_context_.run_one();
    }
    RUVIA_CHECK_EQ(slot.expiry_count_, 1u);
    RUVIA_CHECK(slot.last_expired_ == kind::socket);
    RUVIA_CHECK(slot.deadline_.expired());

    runtime.io_context_.restart();
    asio::post(runtime.io_context_, [&] {
        ruvia::detail::arm_db_slot_deadline(runtime.worker_, slot, 1ms, kind::sleep);
        slot.deadline_continuation_ = std::noop_coroutine();
        ruvia::detail::clear_db_slot_deadline(slot);
        RUVIA_CHECK(!slot.timer_.registered());
        RUVIA_CHECK(!slot.deadline_continuation_);
        RUVIA_CHECK(slot.deadline_.kind() == nullptr);
        RUVIA_CHECK(!slot.deadline_.expired());
        ruvia::detail::arm_db_slot_deadline(runtime.worker_, slot, 1ms, kind::resolve);
        ruvia::detail::arm_db_slot_deadline(runtime.worker_, slot, 0ms, kind::socket);
        RUVIA_CHECK(!slot.timer_.registered());
        RUVIA_CHECK(slot.deadline_.kind() == nullptr);
    });
    runtime.io_context_.poll();
    RUVIA_CHECK_EQ(slot.expiry_count_, 1u);

    runtime.io_context_.restart();
    asio::post(runtime.io_context_, [&] {
        ruvia::detail::arm_db_slot_deadline(runtime.worker_, slot, 1ms, kind::sleep);
        slot.deadline_continuation_ = std::noop_coroutine();
    });
    while (slot.expiry_count_ != 2) {
        runtime.io_context_.run_one();
    }
    RUVIA_CHECK_EQ(slot.expiry_count_, 2u);
    RUVIA_CHECK(slot.last_expired_ == kind::sleep);
    RUVIA_CHECK(!slot.deadline_continuation_);
    ruvia::detail::clear_db_slot_deadline(slot);
}

RUVIA_TEST(db_slot_deadline_initiation_failure_rolls_back_and_can_be_reused) {
    using namespace std::chrono_literals;
    using kind = deadline_test_slot::deadline_kind;
    db_registry_test_runtime runtime;
    deadline_test_slot slot;
    const ruvia::worker_handle unavailable_worker;
    asio::post(runtime.io_context_, [&] {
        ruvia::detail::arm_db_slot_deadline(runtime.worker_, slot, 1h, kind::resolve);
        slot.deadline_continuation_ = std::noop_coroutine();
        RUVIA_CHECK(throws_on([&] {
            ruvia::detail::arm_db_slot_deadline(unavailable_worker, slot, 1ms, kind::sleep);
        }));
        RUVIA_CHECK(!slot.timer_.registered());
        RUVIA_CHECK(!slot.deadline_continuation_);
        RUVIA_CHECK(slot.deadline_.kind() == nullptr);
        RUVIA_CHECK(!slot.deadline_.expired());
        RUVIA_CHECK_EQ(slot.expiry_count_, 0u);
        ruvia::detail::arm_db_slot_deadline(runtime.worker_, slot, 1ms, kind::socket);
    });
    while (slot.expiry_count_ != 1) {
        runtime.io_context_.run_one();
    }
    RUVIA_CHECK_EQ(slot.expiry_count_, 1u);
    RUVIA_CHECK(slot.last_expired_ == kind::socket);
    ruvia::detail::clear_db_slot_deadline(slot);
}

RUVIA_TEST(db_operation_options_validate_and_compose_restrictions) {
    RUVIA_CHECK(throws_on([] {
        ruvia::detail::validate_operation_options(
            ruvia::operation_options{.timeout_ = std::chrono::milliseconds(0)});
    }));
    RUVIA_CHECK(throws_on([] {
        ruvia::detail::validate_operation_options(
            ruvia::operation_options{.timeout_ = std::chrono::milliseconds(-1)});
    }));

    ruvia::stop_source ambient;
    ruvia::stop_source explicit_operation;
    auto merged = ruvia::detail::merge_operation_options(
        ruvia::operation_options{
            .timeout_ = std::chrono::milliseconds(100), .stop_token_ = ambient.token()},
        ruvia::operation_options{
            .timeout_ = std::chrono::milliseconds(250), .stop_token_ = explicit_operation.token()});
    RUVIA_CHECK(merged.timeout_ == std::chrono::milliseconds(100));
    RUVIA_CHECK(!merged.stop_token_.stop_requested());
    explicit_operation.request_stop();
    RUVIA_CHECK(merged.stop_token_.stop_requested());

    ruvia::stop_source second_ambient;
    ruvia::stop_source second_explicit;
    auto shorter_override = ruvia::detail::merge_operation_options(
        ruvia::operation_options{
            .timeout_ = std::chrono::milliseconds(500), .stop_token_ = second_ambient.token()},
        ruvia::operation_options{
            .timeout_ = std::chrono::milliseconds(50), .stop_token_ = second_explicit.token()});
    RUVIA_CHECK(shorter_override.timeout_ == std::chrono::milliseconds(50));
    second_ambient.request_stop();
    RUVIA_CHECK(shorter_override.stop_token_.stop_requested());
}

RUVIA_TEST(db_error_carries_category_and_native_diagnostics) {
    const ruvia::db_error timeout(
        ruvia::db_error::code_type::timeout, ruvia::db_driver::mariadb, "timeout", 1205, "HY000");
    const ruvia::db_error unique_violation(ruvia::db_error::code_type::statement_failed,
        ruvia::db_driver::postgresql, "duplicate key", std::nullopt, "23505", "uq_jobs_key");
    const ruvia::db_error cancelled(
        ruvia::db_error::code_type::cancelled, ruvia::db_driver::postgresql, "cancelled");
    const ruvia::db_error closing(
        ruvia::db_error::code_type::closing, ruvia::db_driver::mariadb, "closing");
    RUVIA_CHECK(timeout.code() == ruvia::db_error::code_type::timeout);
    RUVIA_CHECK(timeout.driver() == ruvia::db_driver::mariadb);
    RUVIA_CHECK(timeout.native_code() == 1205);
    RUVIA_CHECK(timeout.sql_state() == "HY000");
    RUVIA_CHECK(!timeout.constraint_name().has_value());
    RUVIA_CHECK(unique_violation.constraint_name() == "uq_jobs_key");
    RUVIA_CHECK(cancelled.code() == ruvia::db_error::code_type::cancelled);
    RUVIA_CHECK(cancelled.driver() == ruvia::db_driver::postgresql);
    RUVIA_CHECK(!cancelled.native_code().has_value());
    RUVIA_CHECK(!cancelled.sql_state().has_value());
    RUVIA_CHECK(!cancelled.constraint_name().has_value());
    RUVIA_CHECK(closing.code() == ruvia::db_error::code_type::closing);
}

#if defined(RUVIA_ENABLE_MARIADB) || defined(RUVIA_ENABLE_POSTGRESQL)
RUVIA_TEST(db_resolve_shutdown_preserves_slot_until_it_reports_closing) {
    asio::io_context io_context;
    closing_resolve_pool pool;
    closing_resolve_slot slot;
    auto future = asio::co_spawn(io_context,
        ruvia::as_awaitable(ruvia::detail::resolve_db_host(
            pool, slot, ruvia::operation_timeout(std::nullopt), "test database")),
        asio::use_future);
    io_context.run();

    bool reported_closing = false;
    try {
        (void)future.get();
    } catch (const ruvia::db_error& error) {
        reported_closing = error.code() == ruvia::db_error::code_type::closing;
    }
    RUVIA_CHECK(slot.observed_active_resolve_);
    RUVIA_CHECK(!slot.wait_active_);
    RUVIA_CHECK(reported_closing);
}

RUVIA_TEST(db_resolve_initiation_failure_retires_slot_deadline) {
    db_registry_test_runtime runtime;
    closing_resolve_pool pool;
    pool.worker_ = runtime.worker_;
    closing_resolve_slot slot;
    slot.throw_on_initiation_ = true;
    auto future = asio::co_spawn(runtime.io_context_,
        ruvia::as_awaitable(ruvia::detail::resolve_db_host(
            pool, slot, ruvia::operation_timeout(std::chrono::hours(1)), "test database")),
        asio::use_future);
    while (future.wait_for(std::chrono::seconds::zero()) != std::future_status::ready) {
        runtime.io_context_.run_one();
    }
    runtime.io_context_.poll();
    RUVIA_CHECK(throws_on([&] { (void)future.get(); }));
    RUVIA_CHECK(slot.observed_active_resolve_);
    RUVIA_CHECK(!slot.wait_active_);
    RUVIA_CHECK(!slot.timer_.registered());
    RUVIA_CHECK(slot.deadline_.kind() == nullptr);
    RUVIA_CHECK(!slot.deadline_.expired());
}
#endif

#ifdef RUVIA_ENABLE_MARIADB
RUVIA_TEST(mariadb_wait_deadline_uses_the_earliest_source) {
    using namespace std::chrono_literals;
    using ruvia::detail::mysql_wait_deadline_source;

    const auto operation_first = ruvia::detail::select_mysql_wait_deadline(30s, 1s);
    RUVIA_CHECK(operation_first.timeout_ == 1s);
    RUVIA_CHECK(operation_first.source_ == mysql_wait_deadline_source::driver);

    const auto driver_later = ruvia::detail::select_mysql_wait_deadline(1s, 30s);
    RUVIA_CHECK(driver_later.timeout_ == 1s);
    RUVIA_CHECK(driver_later.source_ == mysql_wait_deadline_source::operation);

    const auto tie = ruvia::detail::select_mysql_wait_deadline(1s, 1s);
    RUVIA_CHECK(tie.timeout_ == 1s);
    RUVIA_CHECK(tie.source_ == mysql_wait_deadline_source::operation);

    const auto driver_only = ruvia::detail::select_mysql_wait_deadline(std::nullopt, 2s);
    RUVIA_CHECK(driver_only.timeout_ == 2s);
    RUVIA_CHECK(driver_only.source_ == mysql_wait_deadline_source::driver);
}
#endif

RUVIA_TEST(db_slot_socket_cancel_drains_before_release_and_preserves_driver_socket) {
    asio::io_context io_context;
    asio::ip::tcp::acceptor acceptor(
        io_context, asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0));
    asio::ip::tcp::socket driver_socket(io_context);
    driver_socket.connect(acceptor.local_endpoint());
    asio::ip::tcp::socket peer_socket(io_context);
    acceptor.accept(peer_socket);

    std::error_code driver_release_error;
    const auto source_value = static_cast<ruvia::detail::db_slot_socket::native_socket_type>(
        driver_socket.release(driver_release_error));
    RUVIA_CHECK(!driver_release_error);
    ruvia::detail::db_slot_socket wait_socket(io_context);
    RUVIA_CHECK(!wait_socket.ensure_assigned(source_value));
#if defined(_WIN32)
    RUVIA_CHECK(static_cast<ruvia::detail::db_slot_socket::native_socket_type>(
                    wait_socket.socket_.native_handle()) == source_value);
#else
    RUVIA_CHECK(wait_socket.descriptor_.native_handle() == source_value);
#endif

    int completions = 0;
    std::error_code wait_error;
#if defined(_WIN32)
    wait_socket.socket_.async_wait(asio::ip::tcp::socket::wait_read, [&](std::error_code error) {
        ++completions;
        wait_error = error;
    });
#else
    wait_socket.descriptor_.async_wait(
        asio::posix::stream_descriptor::wait_read, [&](std::error_code error) {
            ++completions;
            wait_error = error;
        });
#endif
    wait_socket.cancel();
    io_context.run();
    RUVIA_CHECK_EQ(completions, 1);
    RUVIA_CHECK(wait_error == asio::error::operation_aborted);
    RUVIA_CHECK(!wait_socket.release());

    std::error_code driver_assign_error;
    driver_socket.assign(asio::ip::tcp::v4(), source_value, driver_assign_error);
    RUVIA_CHECK(!driver_assign_error);
    constexpr std::array<char, 2> payload_value{'o', 'k'};
    std::array<char, payload_value.size()> received_value{};
    asio::write(driver_socket, asio::buffer(payload_value));
    asio::read(peer_socket, asio::buffer(received_value));
    RUVIA_CHECK(received_value == payload_value);
}

RUVIA_TEST(db_slot_socket_reports_invalid_driver_socket) {
    asio::io_context io_context;
    ruvia::detail::db_slot_socket wait_socket(io_context);
    const auto error = wait_socket.ensure_assigned(ruvia::detail::db_slot_socket::invalid_socket);
    RUVIA_CHECK(error == std::errc::bad_file_descriptor);
}

RUVIA_TEST(db_slot_socket_releases_before_driver_socket_closes) {
    asio::io_context io_context;
    asio::ip::tcp::acceptor acceptor(
        io_context, asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0));
    asio::ip::tcp::socket driver_socket(io_context);
    driver_socket.connect(acceptor.local_endpoint());
    asio::ip::tcp::socket peer_socket(io_context);
    acceptor.accept(peer_socket);

    std::error_code driver_release_error;
    const auto source_value = static_cast<ruvia::detail::db_slot_socket::native_socket_type>(
        driver_socket.release(driver_release_error));
    RUVIA_CHECK(!driver_release_error);
    {
        ruvia::detail::db_slot_socket wait_socket(io_context);
        RUVIA_CHECK(!wait_socket.ensure_assigned(source_value));
#if defined(_WIN32)
        RUVIA_CHECK(static_cast<ruvia::detail::db_slot_socket::native_socket_type>(
                        wait_socket.socket_.native_handle()) == source_value);
#else
        RUVIA_CHECK(wait_socket.descriptor_.native_handle() == source_value);
#endif
        RUVIA_CHECK(!wait_socket.release());
        std::error_code driver_assign_error;
        driver_socket.assign(asio::ip::tcp::v4(), source_value, driver_assign_error);
        RUVIA_CHECK(!driver_assign_error);
        std::error_code close_error;
        driver_socket.close(close_error);
        RUVIA_CHECK(!close_error);
    }

    std::array<char, 1> byte{};
    std::error_code read_error;
    (void)peer_socket.read_some(asio::buffer(byte), read_error);
    RUVIA_CHECK(read_error == asio::error::eof || read_error == asio::error::connection_reset);
}

RUVIA_TEST(db_prepared_statement_rejects_blank_sql_before_io) {
    RUVIA_CHECK(throws_on(
        [] { (void)ruvia::prepare_db_statement("", {}, std::pmr::get_default_resource()); }));
    RUVIA_CHECK(throws_on(
        [] { (void)ruvia::prepare_db_statement(" \n\t\r", {}, std::pmr::get_default_resource()); }));

    const auto statement =
        ruvia::prepare_db_statement("SELECT 1", {}, std::pmr::get_default_resource());
    RUVIA_CHECK_EQ(std::string_view(statement.sql_), std::string_view("SELECT 1"));
}

RUVIA_TEST(database_operation_state_cold_borrows_and_start_exclusivity) {
    struct lease final {
        int value_;
    };
    using state_type = ruvia::detail::db_operation_state<lease>;
    using guard_type = ruvia::detail::db_operation_guard<lease>;

    state_type state_value(lease{7});
    guard_type first(state_value);
    guard_type second(state_value);
    RUVIA_CHECK(state_value.active());

    first.start();
    RUVIA_CHECK_EQ(first.lease().value_, 7);
    bool cold_lease_rejected = false;
    try {
        (void)second.lease();
    } catch (const std::logic_error& error) {
        cold_lease_rejected = std::string_view(error.what()) == "database operation is already in progress";
    }
    RUVIA_CHECK(cold_lease_rejected);
    bool overlap_rejected = false;
    try {
        second.start();
    } catch (const std::logic_error& error) {
        overlap_rejected = std::string_view(error.what()) == "database operation is already in progress";
    }
    RUVIA_CHECK(overlap_rejected);
    RUVIA_CHECK_EQ(first.lease().value_, 7);
    first.finish_active();

    second.start();
    RUVIA_CHECK_EQ(second.lease().value_, 7);
    second.finish_active();
    RUVIA_CHECK(state_value.active());
}

RUVIA_TEST(database_operation_guard_drops_cold_borrows_without_claiming_lease) {
    struct payload final {
        int value_;
    };
    using state_type = ruvia::detail::db_operation_state<payload>;
    using guard_type = ruvia::detail::db_operation_guard<payload>;

    state_type state_value(payload{7});
    {
        guard_type first(state_value);
        guard_type second(state_value);
        RUVIA_CHECK(state_value.active());
    }
    RUVIA_CHECK(state_value.active());

    guard_type pending(state_value);
    guard_type failing(state_value);
    failing.start();
    failing.finish_failed();
    bool failed_lease_rejected = false;
    try {
        (void)pending.lease();
    } catch (const std::logic_error& error) {
        failed_lease_rejected = std::string_view(error.what()) == "database resource is not active";
    }
    RUVIA_CHECK(failed_lease_rejected);
    bool failed_start_rejected = false;
    try {
        pending.start();
    } catch (const std::logic_error& error) {
        failed_start_rejected = std::string_view(error.what()) == "database resource is not active";
    }
    RUVIA_CHECK(failed_start_rejected);

    state_type closed_state(payload{9});
    guard_type closed_pending(closed_state);
    guard_type closer(closed_state);
    closer.start();
    closer.finish_closed();
    bool closed_lease_rejected = false;
    try {
        (void)closed_pending.lease();
    } catch (const std::logic_error& error) {
        closed_lease_rejected = std::string_view(error.what()) == "database resource is not active";
    }
    RUVIA_CHECK(closed_lease_rejected);
    bool closed_start_rejected = false;
    try {
        closed_pending.start();
    } catch (const std::logic_error& error) {
        closed_start_rejected = std::string_view(error.what()) == "database resource is not active";
    }
    RUVIA_CHECK(closed_start_rejected);
}

RUVIA_TEST(database_operation_guard_runs_cold_operation) {
    struct payload final {
        int value_;
    };
    struct owner final {
        ruvia::detail::db_operation_state<payload> state_{payload{7}};
    };
    using guard_type = ruvia::detail::db_operation_guard<payload>;
    auto operate = [](guard_type operation, int& observed_value) -> ruvia::task<void> {
        operation.start();
        observed_value = operation.lease().value_;
        operation.finish_active();
        co_return;
    };

    owner owner;
    int observed_value = 0;
    {
        guard_type cold_one(owner.state_);
        guard_type cold_two(owner.state_);
        auto one = operate(std::move(cold_one), observed_value);
        auto two = operate(std::move(cold_two), observed_value);
        RUVIA_CHECK(owner.state_.active());

        asio::io_context io(1);
        auto first = asio::co_spawn(io, ruvia::as_awaitable(std::move(one)), asio::use_future);
        io.run();
        first.get();
        io.restart();
        auto second = asio::co_spawn(io, ruvia::as_awaitable(std::move(two)), asio::use_future);
        io.run();
        second.get();
    }
    RUVIA_CHECK(owner.state_.active());
    RUVIA_CHECK_EQ(observed_value, 7);
}

RUVIA_TEST(database_operation_guarded_cold_tasks_release_owned_inputs_and_retain_results) {
    using lease_type = std::pmr::string;
    using state_type = ruvia::detail::db_operation_state<lease_type>;
    using guard_type = ruvia::detail::db_operation_guard<lease_type>;
    ruvia::test::counting_memory_resource memory;

    auto operate = [](guard_type operation, std::pmr::string input, std::pmr::memory_resource* resource)
        -> ruvia::task<std::pmr::string> {
        operation.start();
        std::pmr::string result(input, resource);
        operation.finish_active();
        co_return result;
    };

    state_type state_value(lease_type("lease"));
    {
        std::optional<std::pmr::string> first_result;
        std::optional<std::pmr::string> second_result;
        auto first = operate(guard_type(state_value), std::pmr::string(256, 'a', &memory), &memory);
        auto second = operate(guard_type(state_value), std::pmr::string(256, 'b', &memory), &memory);
        RUVIA_CHECK_EQ(memory.live_allocations(), 2U * owned_string_allocations());
        {
            auto dropped = operate(guard_type(state_value), std::pmr::string(256, 'x', &memory), &memory);
            RUVIA_CHECK_EQ(memory.live_allocations(), 3U * owned_string_allocations());
        }
        RUVIA_CHECK_EQ(memory.live_allocations(), 2U * owned_string_allocations());
        RUVIA_CHECK(state_value.active());

        asio::io_context io(1);
        {
            auto first_future = asio::co_spawn(io, ruvia::as_awaitable(std::move(first)), asio::use_future);
            io.run();
            first_result.emplace(first_future.get(), &memory);
        }
        RUVIA_CHECK_EQ(first_result->size(), 256U);
        RUVIA_CHECK_EQ(first_result->front(), 'a');
        RUVIA_CHECK_EQ(first_result->back(), 'a');
        RUVIA_CHECK_EQ(memory.live_allocations(), 2U * owned_string_allocations());

        io.restart();
        {
            auto second_future = asio::co_spawn(io, ruvia::as_awaitable(std::move(second)), asio::use_future);
            io.run();
            second_result.emplace(second_future.get(), &memory);
        }
        RUVIA_CHECK_EQ(second_result->size(), 256U);
        RUVIA_CHECK_EQ(second_result->front(), 'b');
        RUVIA_CHECK_EQ(second_result->back(), 'b');
        RUVIA_CHECK_EQ(memory.live_allocations(), 2U * owned_string_allocations());
        RUVIA_CHECK(state_value.active());
    }
    RUVIA_CHECK_EQ(memory.live_allocations(), 0U);
    RUVIA_CHECK_EQ(memory.allocation_count(), memory.deallocation_count());
}

RUVIA_TEST(database_operation_guarded_overlapping_task_does_not_damage_first) {
    using lease_type = std::pmr::string;
    using state_type = ruvia::detail::db_operation_state<lease_type>;
    using guard_type = ruvia::detail::db_operation_guard<lease_type>;
    struct gate final {
        [[nodiscard]] bool await_ready() const noexcept {
            return false;
        }
        void await_suspend(std::coroutine_handle<> continuation) noexcept {
            continuation_ = continuation;
        }
        void await_resume() const noexcept {}
        void resume() noexcept {
            auto continuation = std::exchange(continuation_, {});
            continuation.resume();
        }
        std::coroutine_handle<> continuation_{};
    };
    ruvia::test::counting_memory_resource memory;
    auto operate = [](guard_type operation, gate& gate_value, std::pmr::string input,
                       std::pmr::memory_resource* resource) -> ruvia::task<std::pmr::string> {
        operation.start();
        co_await gate_value;
        std::pmr::string result_value(input, resource);
        operation.finish_active();
        co_return result_value;
    };

    state_type state_value(lease_type("lease"));
    {
        gate gate;
        auto first = operate(guard_type(state_value), gate, std::pmr::string(256, 'a', &memory), &memory);
        auto second = operate(guard_type(state_value), gate, std::pmr::string(256, 'b', &memory), &memory);
        asio::io_context io(1);
        auto first_future = asio::co_spawn(io, ruvia::as_awaitable(std::move(first)), asio::use_future);
        io.poll();
        RUVIA_CHECK(gate.continuation_ != nullptr);

        auto second_future = asio::co_spawn(io, ruvia::as_awaitable(std::move(second)), asio::use_future);
        io.restart();
        // The first co_spawn still owns work while suspended at the gate.
        // Only drain ready handlers for the rejected overlapping operation.
        io.poll();
        bool overlap_rejected = false;
        try {
            (void)second_future.get();
        } catch (const std::logic_error& error) {
            overlap_rejected = std::string_view(error.what()) == "database operation is already in progress";
        }
        RUVIA_CHECK(overlap_rejected);
        RUVIA_CHECK_EQ(memory.live_allocations(), owned_string_allocations());

        io.restart();
        gate.resume();
        io.run();
        const auto result_value = first_future.get();
        RUVIA_CHECK_EQ(result_value.size(), 256U);
        RUVIA_CHECK_EQ(result_value.front(), 'a');
        RUVIA_CHECK(state_value.active());
    }
    RUVIA_CHECK_EQ(memory.live_allocations(), 0U);
    RUVIA_CHECK_EQ(memory.allocation_count(), memory.deallocation_count());
}

RUVIA_TEST(database_operation_guarded_failure_rejects_pending_task_and_releases_inputs) {
    using lease_type = std::pmr::string;
    using state_type = ruvia::detail::db_operation_state<lease_type>;
    using guard_type = ruvia::detail::db_operation_guard<lease_type>;
    ruvia::test::counting_memory_resource memory;

    auto fail = [](guard_type operation, std::pmr::string input) -> ruvia::task<void> {
        operation.start();
        (void)input;
        throw std::runtime_error("operation failed");
        co_return;
    };
    auto complete_value = [](guard_type operation, std::pmr::string input) -> ruvia::task<void> {
        operation.start();
        (void)input;
        operation.finish_active();
        co_return;
    };

    state_type state_value(lease_type("lease"));
    auto failed = fail(guard_type(state_value), std::pmr::string(256, 'f', &memory));
    auto pending = complete_value(guard_type(state_value), std::pmr::string(256, 'p', &memory));
    RUVIA_CHECK_EQ(memory.live_allocations(), 2U * owned_string_allocations());

    asio::io_context io(1);
    auto failed_future = asio::co_spawn(io, ruvia::as_awaitable(std::move(failed)), asio::use_future);
    io.run();
    bool failure_observed = false;
    try {
        failed_future.get();
    } catch (const std::runtime_error& error) {
        failure_observed = std::string_view(error.what()) == "operation failed";
    }
    RUVIA_CHECK(failure_observed);
    RUVIA_CHECK_EQ(memory.live_allocations(), owned_string_allocations());

    io.restart();
    auto pending_future = asio::co_spawn(io, ruvia::as_awaitable(std::move(pending)), asio::use_future);
    io.run();
    bool pending_rejected = false;
    try {
        pending_future.get();
    } catch (const std::logic_error& error) {
        pending_rejected = std::string_view(error.what()) == "database resource is not active";
    }
    RUVIA_CHECK(pending_rejected);
    RUVIA_CHECK_EQ(memory.live_allocations(), 0U);
    RUVIA_CHECK_EQ(memory.allocation_count(), memory.deallocation_count());
}

RUVIA_TEST(database_operation_guarded_started_cancellation_fails_lease_and_releases_inputs) {
    using lease_type = std::pmr::string;
    using state_type = ruvia::detail::db_operation_state<lease_type>;
    using guard_type = ruvia::detail::db_operation_guard<lease_type>;
    ruvia::test::counting_memory_resource memory;
    auto cancel = [](guard_type operation, std::pmr::string input) -> ruvia::task<void> {
        operation.start();
        (void)input;
        throw ruvia::db_error(ruvia::db_error::code_type::cancelled, ruvia::db_driver::postgresql, "cancelled");
        co_return;
    };
    auto complete_value = [](guard_type operation, std::pmr::string input) -> ruvia::task<void> {
        operation.start();
        (void)input;
        operation.finish_active();
        co_return;
    };

    state_type state_value(lease_type("lease"));
    {
        auto cold = cancel(guard_type(state_value), std::pmr::string(256, 'c', &memory));
        auto pending = complete_value(guard_type(state_value), std::pmr::string(256, 'p', &memory));
        RUVIA_CHECK_EQ(memory.live_allocations(), 2U * owned_string_allocations());
        asio::io_context io(1);
        auto future = asio::co_spawn(io, ruvia::as_awaitable(std::move(cold)), asio::use_future);
        io.run();
        bool cancellation_observed = false;
        try {
            future.get();
        } catch (const ruvia::db_error& error) {
            cancellation_observed = error.code() == ruvia::db_error::code_type::cancelled;
        }
        RUVIA_CHECK(cancellation_observed);
        RUVIA_CHECK(!state_value.active());
        RUVIA_CHECK_EQ(memory.live_allocations(), owned_string_allocations());

        io.restart();
        auto pending_future = asio::co_spawn(io, ruvia::as_awaitable(std::move(pending)), asio::use_future);
        io.run();
        bool pending_rejected = false;
        try {
            pending_future.get();
        } catch (const std::logic_error& error) {
            pending_rejected = std::string_view(error.what()) == "database resource is not active";
        }
        RUVIA_CHECK(pending_rejected);
        RUVIA_CHECK_EQ(memory.live_allocations(), 0U);
    }
    RUVIA_CHECK_EQ(memory.allocation_count(), memory.deallocation_count());
}

RUVIA_TEST(database_operation_guard_releases_before_scoped_join_expires_owner) {
    asio::io_context io;
    ruvia::operation_scope scope;
    ruvia::test::counting_memory_resource memory;
    guarded_lease_state_type state_value(guarded_lease_type("lease"));
    bool owner_expired = false;
    guarded_lease_capability capability(scope, state_value, owner_expired);
    guarded_lease_gate gate;

    auto operation = ruvia::make_scoped_operation(scope,
        fail_guarded_lease_after_gate(guarded_lease_guard_type(state_value), gate, std::pmr::string(256, 'j', &memory)));
    auto overlap = ruvia::make_scoped_operation(scope,
        complete_guarded_lease(guarded_lease_guard_type(state_value), std::pmr::string(256, 'o', &memory)));
    auto runner = asio::co_spawn(io,
        ruvia::as_awaitable(await_scoped_operation(operation)), asio::use_future);
    io.poll();
    RUVIA_CHECK(gate.continuation_ != nullptr);
    RUVIA_CHECK_EQ(memory.live_allocations(), 2U * owned_string_allocations());

    auto overlap_runner = asio::co_spawn(io,
        ruvia::as_awaitable(await_scoped_operation(overlap)), asio::use_future);
    io.restart();
    io.poll();
    bool overlap_rejected = false;
    try {
        overlap_runner.get();
    } catch (const std::logic_error& error) {
        overlap_rejected = std::string_view(error.what()) == "database operation is already in progress";
    }
    RUVIA_CHECK(overlap_rejected);
    RUVIA_CHECK_EQ(memory.live_allocations(), owned_string_allocations());

    auto joiner = asio::co_spawn(io,
        ruvia::as_awaitable(join_scoped_operations(scope)), asio::use_future);
    io.restart();
    io.poll();
    RUVIA_CHECK(!scope.active());
    RUVIA_CHECK(!owner_expired);

    gate.resume();
    io.restart();
    io.run();
    bool operation_failed = false;
    try {
        runner.get();
    } catch (const std::runtime_error& error) {
        operation_failed = std::string_view(error.what()) == "scoped database operation failed";
    }
    joiner.get();

    RUVIA_CHECK(operation_failed);
    RUVIA_CHECK(owner_expired);
    RUVIA_CHECK(!state_value.active());
    RUVIA_CHECK_EQ(memory.live_allocations(), 0U);
    RUVIA_CHECK_EQ(memory.allocation_count(), memory.deallocation_count());
}

RUVIA_TEST(database_operation_guard_survives_moving_stable_owner_while_running) {
    struct payload final {
        int value_;
    };
    using state_type = ruvia::detail::db_operation_state<payload>;
    using guard_type = ruvia::detail::db_operation_guard<payload>;

    struct resume_gate final {
        [[nodiscard]] bool await_ready() const noexcept {
            return false;
        }

        void await_suspend(std::coroutine_handle<> continuation) noexcept {
            continuation_ = continuation;
        }

        void await_resume() const noexcept {}

        void resume() noexcept {
            auto continuation = std::exchange(continuation_, {});
            continuation.resume();
        }

        std::coroutine_handle<> continuation_{};
    };

    struct owner final {
        owner()
            : state_(std::make_unique<state_type>(payload{7})) {}

        owner(const owner&) = delete;
        owner& operator=(const owner&) = delete;
        owner(owner&&) noexcept = default;

        std::unique_ptr<state_type> state_;
    };

    auto operate = [](guard_type operation, resume_gate& gate_value, int& observed_value) -> ruvia::task<void> {
        operation.start();
        co_await gate_value;
        observed_value = operation.lease().value_;
        operation.finish_active();
    };

    owner source;
    resume_gate gate;
    int observed_value = 0;
    guard_type reservation(*source.state_);
    auto task_value = operate(std::move(reservation), gate, observed_value);

    asio::io_context io(1);
    auto future =
        asio::co_spawn(io, ruvia::as_awaitable(std::move(task_value)), asio::use_future);
    io.poll();
    RUVIA_CHECK(gate.continuation_ != nullptr);

    owner moved(std::move(source));
    RUVIA_CHECK(source.state_ == nullptr);
    io.restart();
    gate.resume();
    io.run();
    future.get();

    RUVIA_CHECK_EQ(observed_value, 7);
    RUVIA_CHECK(moved.state_->active());
}

RUVIA_TEST(scoped_operation_scope_tracks_cold_owner_operations) {
    ruvia::operation_scope operation_scope;
    auto cold_task = []() -> ruvia::task<void> { co_return; }();
    {
        auto operation = ruvia::make_scoped_operation(operation_scope, std::move(cold_task));
        RUVIA_CHECK(operation_scope.has_pending_operations());
    }
    RUVIA_CHECK(!operation_scope.has_pending_operations());
}

RUVIA_TEST(db_value_and_result_storage_have_one_live_alternative) {
    const ruvia::db_value null_value(nullptr);
    const ruvia::db_value text_value("value");
    const ruvia::db_value borrowed_text_value(ruvia::borrowed_text("borrowed-value"));
    const ruvia::db_value signed_value(-7);
    const ruvia::db_value unsigned_value(std::uint64_t{9});
    const ruvia::db_value double_value_value(1.5);
    const ruvia::db_value bool_value_value(true);
    using value_access_type = ruvia::detail::db_value_access;
    RUVIA_CHECK(value_access_type::type(null_value) == ruvia::detail::db_value_type::null);
    RUVIA_CHECK(value_access_type::type(text_value) == ruvia::detail::db_value_type::string);
    RUVIA_CHECK_EQ(value_access_type::text(text_value), std::string_view("value"));
    RUVIA_CHECK(value_access_type::type(borrowed_text_value) == ruvia::detail::db_value_type::string);
    RUVIA_CHECK_EQ(value_access_type::text(borrowed_text_value), std::string_view("borrowed-value"));
    RUVIA_CHECK(value_access_type::type(signed_value) == ruvia::detail::db_value_type::signed_value);
    RUVIA_CHECK_EQ(value_access_type::signed_value(signed_value), std::int64_t{-7});
    RUVIA_CHECK(value_access_type::type(unsigned_value) == ruvia::detail::db_value_type::unsigned_value);
    RUVIA_CHECK_EQ(value_access_type::unsigned_value(unsigned_value), std::uint64_t{9});
    RUVIA_CHECK(value_access_type::type(double_value_value) == ruvia::detail::db_value_type::double_value);
    RUVIA_CHECK_EQ(value_access_type::get_double_value(double_value_value), 1.5);
    RUVIA_CHECK(value_access_type::type(bool_value_value) == ruvia::detail::db_value_type::bool_value);
    RUVIA_CHECK(value_access_type::get_bool_value(bool_value_value));

    auto owned_row = ruvia::detail::db_result_access::owned_row(nullptr);
    auto& fields_value = ruvia::detail::db_result_access::owned_fields(owned_row);
    auto& column_names = ruvia::detail::db_result_access::owned_column_names(owned_row);
    column_names.emplace_back("label");
    fields_value.push_back(ruvia::detail::db_result_access::owned_field("owned", nullptr));
    RUVIA_CHECK_EQ(owned_row.size(), std::size_t{1});
    RUVIA_CHECK(owned_row[0].value() == std::optional<std::string_view>("owned"));
    RUVIA_CHECK(owned_row["label"].as<std::string>() == std::optional<std::string>("owned"));

    auto moved_row = std::move(owned_row);
    RUVIA_CHECK(owned_row.empty());
    RUVIA_CHECK_EQ(moved_row.size(), std::size_t{1});

    auto borrowed_field = ruvia::detail::db_result_access::borrowed_field("borrowed", nullptr);
    const std::pmr::string borrowed_column("borrowed_column");
    auto borrowed_row =
        ruvia::detail::db_result_access::borrowed_row(&borrowed_field, 1, &borrowed_column, 1, nullptr);
    RUVIA_CHECK(borrowed_row["borrowed_column"].as<std::string_view>() ==
                std::optional<std::string_view>("borrowed"));

    auto moved_field = std::move(borrowed_field);
    // db_field defines an observable empty moved-from state; this assertion is
    // the contract under test rather than an accidental post-move use.
    RUVIA_CHECK(!borrowed_field.value().has_value());  // NOLINT(clang-analyzer-cplusplus.Move)
    RUVIA_CHECK(moved_field.value() == std::optional<std::string_view>("borrowed"));
    auto numeric = ruvia::detail::db_result_access::owned_field("-42", nullptr);
    RUVIA_CHECK(numeric.as<std::int64_t>() == std::optional<std::int64_t>(-42));
    auto boolean = ruvia::detail::db_result_access::owned_field("t", nullptr);
    RUVIA_CHECK(boolean.as<bool>() == std::optional<bool>(true));
    auto null = ruvia::detail::db_result_access::null_field(nullptr);
    RUVIA_CHECK(!null.as<std::int64_t>().has_value());
    auto invalid = ruvia::detail::db_result_access::owned_field("not-a-number", nullptr);
    try {
        (void)invalid.as<std::int64_t>();
        RUVIA_CHECK(false);
    } catch (const ruvia::db_conversion_error& error) {
        RUVIA_CHECK(error.code() == ruvia::db_conversion_error::code_type::invalid_format);
    }

    try {
        (void)moved_row["missing"];
        RUVIA_CHECK(false);
    } catch (const std::out_of_range&) {
    }
}

RUVIA_TEST(db_query_rows_and_execution_metadata_have_independent_storage) {
    int releases = 0;
    {
        auto result_value = ruvia::detail::db_result_access::make_result(nullptr);
        auto& column_names = ruvia::detail::db_result_access::column_names(result_value);
        auto& fields_value = ruvia::detail::db_result_access::fields(result_value);
        auto& rows = ruvia::detail::db_result_access::rows(result_value);
        column_names.emplace_back("value");
        fields_value.push_back(ruvia::detail::db_result_access::borrowed_field("stable", nullptr));
        rows.push_back(ruvia::detail::db_result_access::borrowed_row(
            fields_value.data(), fields_value.size(), column_names.data(), column_names.size(), nullptr));
        ruvia::detail::db_result_access::own_raw_result(
            result_value, &releases, [](void* value) noexcept { ++*static_cast<int*>(value); });
        const auto execution = ruvia::detail::db_result_access::make_exec_result(7);

        auto moved = std::move(result_value);
        RUVIA_CHECK_EQ(execution.affected_rows(), std::uint64_t{7});
        RUVIA_CHECK(!execution.last_insert_id().has_value());
        RUVIA_CHECK_EQ(moved.size(), std::size_t{1});
        RUVIA_CHECK(moved[0]["value"].value() == std::optional<std::string_view>("stable"));
        RUVIA_CHECK_EQ(releases, 0);
    }
    RUVIA_CHECK_EQ(releases, 1);
}

RUVIA_TEST(db_registry_derives_default_pool_from_owned_entry_index) {
    db_registry_test_runtime runtime;
#ifdef RUVIA_ENABLE_MARIADB
    const auto config = ruvia::db_config{.driver_ = ruvia::db_driver::mariadb};
#else
    const auto config = ruvia::db_config{.driver_ = ruvia::db_driver::postgresql};
#endif
    const std::array<ruvia::detail::db_definition, 2> definitions{{
        db_definition("analytics", config),
        db_definition("default", config),
    }};
    ruvia::detail::db_registry registry(
        runtime.io_context_, runtime.worker_, std::pmr::get_default_resource(), definitions);
    ruvia::operation_scope operation_scope;

    bool default_resolved = true;
    bool alias_resolved = true;
    try {
        (void)registry.get(operation_scope);
    } catch (...) {
        default_resolved = false;
    }
    try {
        (void)registry.get("analytics", operation_scope);
    } catch (...) {
        alias_resolved = false;
    }
    RUVIA_CHECK(default_resolved);
    RUVIA_CHECK(alias_resolved);
}

RUVIA_TEST(db_registry_reports_typed_not_configured_error) {
    db_registry_test_runtime runtime;
    ruvia::detail::db_registry registry(runtime.io_context_, runtime.worker_,
        std::pmr::get_default_resource(), std::span<const ruvia::detail::db_definition>());
    ruvia::operation_scope operation_scope;

    bool default_typed = false;
    bool alias_typed = false;
    try {
        (void)registry.get(operation_scope);
    } catch (const ruvia::db_error& error) {
        default_typed =
            error.code() == ruvia::db_error::code_type::not_configured && !error.driver().has_value();
    }
    try {
        (void)registry.get("missing", operation_scope);
    } catch (const ruvia::db_error& error) {
        alias_typed =
            error.code() == ruvia::db_error::code_type::not_configured && !error.driver().has_value();
    }
    RUVIA_CHECK(default_typed);
    RUVIA_CHECK(alias_typed);
}

RUVIA_TEST(db_registry_owns_nested_pmr_configuration) {
    tracking_resource source_resource;
    std::pmr::unsynchronized_pool_resource target_resource;
    db_registry_test_runtime runtime;
    std::optional<ruvia::detail::db_definition> definition;
    auto config = test_db_config();
    config.host_ = std::string(80, 'h');
    config.tls_.mode_ = ruvia::client_tls_mode::disabled;
    config.username_ = std::string(80, 'u');
    config.password_ = std::string(80, 'p');
    config.database_ = std::string(80, 'd');
    definition.emplace(db_definition("default", config, &source_resource));

    std::optional<ruvia::detail::db_registry> registry;
    registry.emplace(runtime.io_context_, runtime.worker_, &target_resource,
        std::span<const ruvia::detail::db_definition>(&*definition, 1));
    definition.reset();
    source_resource.release();
    registry.reset();

    RUVIA_CHECK(!source_resource.deallocated_after_release());
}

RUVIA_TEST(db_handle_copy_rejects_after_parent_scope_closes) {
    db_registry_test_runtime runtime;
#ifdef RUVIA_ENABLE_MARIADB
    const auto config = ruvia::db_config{.driver_ = ruvia::db_driver::mariadb};
#else
    const auto config = ruvia::db_config{.driver_ = ruvia::db_driver::postgresql};
#endif
    const std::array definitions{db_definition("default", config)};
    ruvia::detail::db_registry registry(
        runtime.io_context_, runtime.worker_, std::pmr::get_default_resource(), definitions);
    ruvia::operation_scope operation_scope;
    auto handle = registry.get(operation_scope);
    auto copied_handle = handle;
    operation_scope.close();

    bool handle_rejected = false;
    bool copy_rejected = false;
    try {
        (void)handle.query("SELECT 1");
    } catch (const std::logic_error&) {
        handle_rejected = true;
    }
    try {
        (void)copied_handle.query("SELECT 1");
    } catch (const std::logic_error&) {
        copy_rejected = true;
    }
    RUVIA_CHECK(handle_rejected);
    RUVIA_CHECK(copy_rejected);
}

RUVIA_TEST(db_migrator_validates_before_opening_connection) {
    const std::array<ruvia::db_migration, 2> migrations{{
        ruvia::db_migration{{.id_ = "duplicate", .sql_ = "SELECT 1"}},
        ruvia::db_migration{{.id_ = "duplicate", .sql_ = "SELECT 2"}},
    }};
    bool rejected = false;
    try {
        (void)ruvia::db_migrator::migrate(
            test_db_config(), std::span<const ruvia::db_migration>(migrations));
    } catch (const std::invalid_argument& error) {
        rejected = std::string_view(error.what()) ==
                   "database migration ids must be unique, including case";
    }
    RUVIA_CHECK(rejected);
}

#ifdef RUVIA_ENABLE_POSTGRESQL
RUVIA_TEST(db_migrator_rejects_unrepresentable_postgresql_lock_timeout_before_connecting) {
    auto config = ruvia::db_config{.driver_ = ruvia::db_driver::postgresql};
    ruvia::db_migrator_options options;
    options.lock_timeout_ = std::chrono::seconds::max();

    bool rejected = false;
    try {
        (void)ruvia::db_migrator::migrate(config, std::span<const ruvia::db_migration>(), options);
    } catch (const std::invalid_argument& error) {
        rejected =
            std::string_view(error.what()) ==
            "database migration lock timeout cannot be represented as PostgreSQL milliseconds";
    }
    RUVIA_CHECK(rejected);
}
#endif

RUVIA_TEST(db_migrator_copies_public_configuration) {
    tracking_resource target_resource;
    std::optional<ruvia::db_migrator> migrator;
    {
        auto config = test_db_config();
        config.host_ = std::string(80, 'h');
        config.tls_.mode_ = ruvia::client_tls_mode::disabled;
        config.username_ = std::string(80, 'u');
        config.password_ = std::string(80, 'p');
        config.database_ = std::string(80, 'd');
        ruvia::db_migrator_options options{
            .table_ = std::string(60, 't'),
            .lock_timeout_ = std::chrono::seconds(30),
            .resource_ = &target_resource,
        };
        migrator.emplace(config, options);
    }
    // Opaque owner + four long DB strings + the long migration table all use
    // the caller's resource rather than their public std::string allocators.
    RUVIA_CHECK(target_resource.allocation_count() >= 6);
    migrator.reset();
}

RUVIA_TEST(db_migrator_validates_complete_configuration_before_allocating) {
    {
        auto config = test_db_config();
        config.host_ = std::string(80, 'h');
        config.tls_.mode_ = ruvia::client_tls_mode::disabled;
        config.connect_timeout_ = std::chrono::milliseconds::zero();
        tracking_resource resource;
        const ruvia::db_migrator_options options{
            .table_ = std::string(60, 't'),
            .resource_ = &resource,
        };

        RUVIA_CHECK(throws_on([&] { (void)ruvia::db_migrator(config, options); }));
        RUVIA_CHECK_EQ(resource.allocation_count(), std::size_t{0});
    }
    {
        auto config = test_db_config();
        config.host_ = std::string(80, 'h');
        config.tls_.mode_ = ruvia::client_tls_mode::disabled;
        tracking_resource resource;
        const ruvia::db_migrator_options options{
            .table_ = "invalid-table-name",
            .resource_ = &resource,
        };

        RUVIA_CHECK(throws_on([&] { (void)ruvia::db_migrator(config, options); }));
        RUVIA_CHECK_EQ(resource.allocation_count(), std::size_t{0});
    }
}

RUVIA_TEST(db_migrator_validates_migration_list_before_allocating_runtime) {
    const std::array<ruvia::db_migration, 2> migrations{{
        ruvia::db_migration{{.id_ = "duplicate", .sql_ = "SELECT 1"}},
        ruvia::db_migration{{.id_ = "duplicate", .sql_ = "SELECT 2"}},
    }};
    auto config = test_db_config();
    config.host_ = std::string(80, 'h');
    config.tls_.mode_ = ruvia::client_tls_mode::disabled;
    tracking_resource resource;
    const ruvia::db_migrator_options options{
        .table_ = std::string(60, 't'),
        .resource_ = &resource,
    };

    RUVIA_CHECK(throws_on([&] {
        (void)ruvia::db_migrator::migrate(
            config, std::span<const ruvia::db_migration>(migrations), options);
    }));
    RUVIA_CHECK_EQ(resource.allocation_count(), std::size_t{0});
}

RUVIA_TEST(db_result_value_move_assignment_propagates_allocator_failure) {
    rejecting_memory_resource rejecting;
    auto destination = ruvia::detail::db_result_access::owned_field({}, &rejecting);
    auto source_value = ruvia::detail::db_result_access::owned_field(
        std::string_view("database field large enough to require an allocation"),
        std::pmr::get_default_resource());
    rejecting.reject_allocations();

    bool allocation_failure = false;
    try {
        destination = std::move(source_value);
    } catch (const std::bad_alloc&) {
        allocation_failure = true;
    }
    RUVIA_CHECK(allocation_failure);

    rejecting.reject_allocations(false);
    auto destination_row = ruvia::detail::db_result_access::owned_row(&rejecting);
    auto source_row = ruvia::detail::db_result_access::owned_row(std::pmr::get_default_resource());
    ruvia::detail::db_result_access::owned_fields(source_row).emplace_back(ruvia::detail::db_result_access::owned_field("row field", std::pmr::get_default_resource()));
    rejecting.reject_allocations();

    allocation_failure = false;
    try {
        destination_row = std::move(source_row);
    } catch (const std::bad_alloc&) {
        allocation_failure = true;
    }
    RUVIA_CHECK(allocation_failure);
}

RUVIA_TEST(db_row_move_assignment_preserves_destination_on_allocation_failure) {
    rejecting_memory_resource destination_resource;
    auto destination = ruvia::detail::db_result_access::owned_row(&destination_resource);
    auto& destination_fields = ruvia::detail::db_result_access::owned_fields(destination);
    auto& destination_names = ruvia::detail::db_result_access::owned_column_names(destination);
    destination_fields.push_back(ruvia::detail::db_result_access::owned_field("old first", &destination_resource));
    destination_fields.push_back(ruvia::detail::db_result_access::owned_field("old second", &destination_resource));
    destination_names.emplace_back("first");
    destination_names.emplace_back("second");

    auto source_value = ruvia::detail::db_result_access::owned_row(std::pmr::get_default_resource());
    ruvia::detail::db_result_access::owned_fields(source_value).push_back(ruvia::detail::db_result_access::owned_field("incoming", std::pmr::get_default_resource()));
    ruvia::detail::db_result_access::owned_column_names(source_value).emplace_back(64, 'n');
    destination_resource.reject_allocations();

    bool allocation_failed = false;
    try {
        destination = std::move(source_value);
    } catch (const std::bad_alloc&) {
        allocation_failed = true;
    }
    RUVIA_CHECK(allocation_failed);
    RUVIA_CHECK_EQ(destination.size(), std::size_t{2});
    if (destination.size() != 2) {
        return;
    }
    RUVIA_CHECK_EQ(destination["first"].value().value_or("missing"), std::string_view("old first"));
    RUVIA_CHECK_EQ(destination["second"].value().value_or("missing"), std::string_view("old second"));
}

RUVIA_TEST(db_row_move_assignment_owns_fields_in_destination_resource) {
    const std::string column(64, 'c');
    const std::string value(128, 'v');
    const std::string borrowed_value(128, 'b');
    for (const bool same_resource : {false, true}) {
        ruvia::test::counting_memory_resource source_resource;
        ruvia::test::counting_memory_resource destination_resource;
        {
            auto destination = ruvia::detail::db_result_access::owned_row(&destination_resource);
            {
                auto* const resource = same_resource ? &destination_resource : &source_resource;
                auto source_value = ruvia::detail::db_result_access::owned_row(resource);
                auto& fields_value = ruvia::detail::db_result_access::owned_fields(source_value);
                auto& names = ruvia::detail::db_result_access::owned_column_names(source_value);
                fields_value.push_back(ruvia::detail::db_result_access::owned_field(value, resource));
                fields_value.push_back(ruvia::detail::db_result_access::borrowed_field(borrowed_value, resource));
                fields_value.push_back(ruvia::detail::db_result_access::null_field(resource));
                names.emplace_back(column);
                names.emplace_back("borrowed");
                names.emplace_back("null");
                destination = std::move(source_value);
                RUVIA_CHECK(source_value.empty());
            }
            RUVIA_CHECK_EQ(source_resource.live_allocations(), std::size_t{0});
            RUVIA_CHECK_EQ(destination.size(), std::size_t{3});
            RUVIA_CHECK_EQ(destination[column].value().value_or("missing"), std::string_view(value));
            const auto borrowed = destination["borrowed"].value();
            RUVIA_CHECK_EQ(borrowed.value_or("missing"), std::string_view(borrowed_value));
            RUVIA_CHECK(borrowed && borrowed->data() == borrowed_value.data());
            RUVIA_CHECK(!destination["null"].value().has_value());
        }
        RUVIA_CHECK_EQ(destination_resource.live_allocations(), std::size_t{0});
        RUVIA_CHECK_EQ(source_resource.live_allocations(), std::size_t{0});
    }
}

RUVIA_TEST(db_row_move_assignment_preserves_borrowed_row_views) {
    ruvia::test::counting_memory_resource backing_resource;
    ruvia::test::counting_memory_resource destination_resource;
    const std::string value(128, 'v');
    {
        auto backing = ruvia::detail::db_result_access::make_result(&backing_resource);
        auto& fields_value = ruvia::detail::db_result_access::fields(backing);
        auto& names = ruvia::detail::db_result_access::column_names(backing);
        fields_value.push_back(ruvia::detail::db_result_access::borrowed_field(value, &backing_resource));
        fields_value.push_back(ruvia::detail::db_result_access::null_field(&backing_resource));
        names.emplace_back("value");
        names.emplace_back("null");
        auto source_value = ruvia::detail::db_result_access::borrowed_row(
            fields_value.data(), fields_value.size(), names.data(), names.size(), &backing_resource);
        auto destination = ruvia::detail::db_result_access::owned_row(&destination_resource);
        destination = std::move(source_value);
        RUVIA_CHECK(source_value.empty());
        RUVIA_CHECK_EQ(destination.size(), std::size_t{2});
        const auto borrowed = destination["value"].value();
        RUVIA_CHECK_EQ(borrowed.value_or("missing"), std::string_view(value));
        RUVIA_CHECK(borrowed && borrowed->data() == value.data());
        RUVIA_CHECK(!destination["null"].value().has_value());
    }
    RUVIA_CHECK_EQ(backing_resource.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(destination_resource.live_allocations(), std::size_t{0});
}

namespace {
struct default_resource_guard final {
    explicit default_resource_guard(std::pmr::memory_resource* resource) noexcept
        : previous_(std::pmr::set_default_resource(resource)) {}
    ~default_resource_guard() {
        std::pmr::set_default_resource(previous_);
    }
    std::pmr::memory_resource* previous_;
};
}  // namespace

RUVIA_TEST(db_expression_values_use_explicit_resource_for_owned_parameters) {
    ruvia::test::counting_memory_resource source_resource;
    const std::string value(128, 'v');
    ruvia::db_query source_value(&source_resource);
    source_value.select(source_value.value(value));
    const auto statement = source_value.compile(ruvia::db_driver::postgresql, &source_resource);
    RUVIA_CHECK_EQ(statement.params().size(), std::size_t{1});
    if (statement.params().size() != 1) {
        return;
    }

    for (const bool through_expressions : {false, true}) {
        ruvia::test::counting_memory_resource destination_resource;
        ruvia::db_query destination(&destination_resource);
        ruvia::db_expressions expressions(&destination_resource);
        rejecting_memory_resource rejecting_default;
        rejecting_default.reject_allocations();
        bool unexpected_allocation = false;
        try {
            default_resource_guard guard_value(&rejecting_default);
            destination.select(through_expressions
                                   ? destination.import_expression(expressions.value(statement.params()[0]))
                                   : destination.value(statement.params()[0]));
        } catch (const std::bad_alloc&) {
            unexpected_allocation = true;
        }
        RUVIA_CHECK(!unexpected_allocation);
        if (!unexpected_allocation) {
            const auto compiled = destination.compile(ruvia::db_driver::postgresql, &destination_resource);
            RUVIA_CHECK_EQ(compiled.params().size(), std::size_t{1});
            if (compiled.params().size() == 1) {
                RUVIA_CHECK_EQ(ruvia::detail::db_value_access::text(compiled.params()[0]), std::string_view(value));
            }
        }
    }
}

RUVIA_TEST(db_variadic_calls_use_explicit_resource_for_owned_parameters) {
    ruvia::test::counting_memory_resource source_resource;
    const std::string value(128, 'v');
    ruvia::db_query source_value(&source_resource);
    source_value.select(source_value.value(value));
    const auto statement = source_value.compile(ruvia::db_driver::postgresql, &source_resource);
    RUVIA_CHECK_EQ(statement.params().size(), std::size_t{1});
    if (statement.params().size() != 1) {
        return;
    }

    db_registry_test_runtime runtime;
    ruvia::test::counting_memory_resource destination_resource;
    const auto config = test_db_config();
    ruvia::detail::db_registry registry(runtime.io_context_, runtime.worker_, &destination_resource, config);
    ruvia::operation_scope scope;
    auto handle = registry.get(scope);
    const std::string_view sql = config.driver_ == ruvia::db_driver::postgresql ? "SELECT $1" : "SELECT ?";
    const auto baseline = destination_resource.live_allocations();

    for (int operation_kind = 0; operation_kind != 3; ++operation_kind) {
        rejecting_memory_resource rejecting_default;
        rejecting_default.reject_allocations();
        bool unexpected_allocation = false;
        try {
            default_resource_guard guard_value(&rejecting_default);
            if (operation_kind == 0) {
                auto operation = handle.query(sql, statement.params()[0]);
                RUVIA_CHECK(destination_resource.live_allocations() > baseline);
                static_cast<void>(operation);
            } else if (operation_kind == 1) {
                auto operation = handle.execute(sql, statement.params()[0]);
                RUVIA_CHECK(destination_resource.live_allocations() > baseline);
                static_cast<void>(operation);
            } else {
                auto operation = handle.query_stream(sql, statement.params()[0]);
                RUVIA_CHECK(destination_resource.live_allocations() > baseline);
                static_cast<void>(operation);
            }
        } catch (const std::bad_alloc&) {
            unexpected_allocation = true;
        }
        RUVIA_CHECK(!unexpected_allocation);
        RUVIA_CHECK_EQ(destination_resource.live_allocations(), baseline);
    }
}

RUVIA_TEST(db_sql_literal_cold_operations_release_owned_parameters) {
    db_registry_test_runtime runtime;
    ruvia::test::counting_memory_resource memory;
    ruvia::detail::db_registry registry(runtime.io_context_, runtime.worker_, &memory, test_db_config());
    ruvia::operation_scope scope;
    auto handle = registry.get(scope);
    const auto baseline = memory.live_allocations();
    for (int i = 0; i < 16; ++i) {
        {
#ifdef RUVIA_ENABLE_MARIADB
            auto query = handle.query<"SELECT ?">(std::string(200, 'q'));
            auto execute = handle.execute<"UPDATE t SET name = ?">(std::string(200, 'e'));
            auto stream = handle.query_stream<"SELECT ?">(std::string(200, 's'));
            auto no_params = handle.query<"SELECT 1">();
#else
            auto query = handle.query<"SELECT $1", ruvia::db_driver::postgresql>(std::string(200, 'q'));
            auto execute = handle.execute<"UPDATE t SET name = $1", ruvia::db_driver::postgresql>(std::string(200, 'e'));
            auto stream = handle.query_stream<"SELECT $1", ruvia::db_driver::postgresql>(std::string(200, 's'));
            auto no_params = handle.query<"SELECT 1", ruvia::db_driver::postgresql>();
#endif
            RUVIA_CHECK(memory.live_allocations() > baseline);
        }
        RUVIA_CHECK_EQ(memory.live_allocations(), baseline);
    }
    const auto allocations = memory.allocation_count();
    const bool rejected = throws_on([&] {
#ifdef RUVIA_ENABLE_MARIADB
        (void)handle.query<"SELECT $1", ruvia::db_driver::postgresql>(1);
#else
        (void)handle.query<"SELECT ?">(1);
#endif
    });
    RUVIA_CHECK(rejected);
    RUVIA_CHECK_EQ(memory.allocation_count(), allocations);
}

RUVIA_TEST(database_tls_configuration_enforces_backend_identity_constraints) {
#ifdef RUVIA_ENABLE_MARIADB
    ruvia::db_config maria{.driver_ = ruvia::db_driver::mariadb};
    RUVIA_CHECK(maria.tls_.mode_ == ruvia::client_tls_mode::verify_identity);
    RUVIA_CHECK(!ruvia::testing::throws_on([&] { ruvia::detail::validate_db_config(maria); }));
    maria.host_ = "database.example.test";
    RUVIA_CHECK(ruvia::testing::throws_on([&] { ruvia::detail::validate_db_config(maria); }));
    maria.host_ = "127.0.0.1";
    maria.tls_.server_name_ = "database.example.test";
    RUVIA_CHECK(ruvia::testing::throws_on([&] { ruvia::detail::validate_db_config(maria); }));
    maria.tls_ = {.mode_ = ruvia::client_tls_mode::disabled};
    maria.host_ = "database.example.test";
    RUVIA_CHECK(!ruvia::testing::throws_on([&] { ruvia::detail::validate_db_config(maria); }));
#endif
#ifdef RUVIA_ENABLE_POSTGRESQL
    ruvia::db_config postgres{.driver_ = ruvia::db_driver::postgresql, .host_ = "database.example.test"};
    postgres.tls_.server_name_ = "expected.example.test";
    RUVIA_CHECK(!ruvia::testing::throws_on([&] { ruvia::detail::validate_db_config(postgres); }));
    postgres.tls_.ca_file_ = std::string("bad\0file", 8);
    RUVIA_CHECK(ruvia::testing::throws_on([&] { ruvia::detail::validate_db_config(postgres); }));
#endif
}

RUVIA_TEST(db_row_move_assignment_owns_fields_in_destination_resource_after_source_resource_release) {
    using access = ruvia::detail::db_result_access;
    const std::string first_value(256, 'a');
    const std::string second_value(256, 'b');
    constexpr std::string_view borrowed = "borrowed";
    for (const bool populated : {false, true}) {
        tracking_resource source_resource;
        ruvia::test::counting_memory_resource destination_resource;
        {
            auto destination = access::owned_row(&destination_resource);
            if (populated) {
                access::owned_fields(destination).push_back(access::owned_field("old", &destination_resource));
                access::owned_column_names(destination).emplace_back("old");
            }
            {
                auto source_value = access::owned_row(&source_resource);
                auto& fields_value = access::owned_fields(source_value);
                fields_value.push_back(access::owned_field(first_value, &source_resource));
                fields_value.push_back(access::owned_field(second_value, &source_resource));
                fields_value.push_back(access::borrowed_field(borrowed, &source_resource));
                fields_value.push_back(access::null_field(&source_resource));
                auto& names = access::owned_column_names(source_value);
                for (const auto name : {"first", "second", "borrowed", "null"}) {
                    names.emplace_back(name);
                }
                destination = std::move(source_value);
                RUVIA_CHECK(source_value.empty());
            }
            source_resource.release();
            RUVIA_CHECK(destination["first"].value() == std::optional<std::string_view>(first_value));
            RUVIA_CHECK(destination["second"].value() == std::optional<std::string_view>(second_value));
            RUVIA_CHECK(destination["borrowed"].value()->data() == borrowed.data());
            RUVIA_CHECK(!destination["null"].value());
        }
        RUVIA_CHECK(!source_resource.deallocated_after_release());
        RUVIA_CHECK_EQ(destination_resource.live_allocations(), std::size_t{0});
    }
}

RUVIA_TEST(db_row_move_assignment_keeps_rows_consistent_when_allocation_fails) {
    using access = ruvia::detail::db_result_access;
    for (const bool fail_field : {false, true}) {
        const std::string incoming_value = fail_field ? std::string(256, 'v') : "new";
        rejecting_memory_resource destination_resource;
        auto destination = access::owned_row(&destination_resource);
        auto& destination_fields = access::owned_fields(destination);
        auto& destination_names = access::owned_column_names(destination);
        for (const auto name : {"old_first", "old_second"}) {
            destination_fields.push_back(access::owned_field(name, &destination_resource));
            destination_names.emplace_back(name);
        }
        auto source_value = access::owned_row(std::pmr::get_default_resource());
        access::owned_fields(source_value).push_back(access::owned_field(incoming_value, std::pmr::get_default_resource()));
        access::owned_column_names(source_value).emplace_back(std::string(256, 'n'));
        destination_resource.reject_allocations(true, 256);

        bool allocation_failed = false;
        try {
            destination = std::move(source_value);
        } catch (const std::bad_alloc&) {
            allocation_failed = true;
        }
        RUVIA_CHECK(allocation_failed);
        RUVIA_CHECK_EQ(destination.size(), std::size_t{2});
        RUVIA_CHECK_EQ(access::column_names(destination).size(), destination.size());
        if (destination.size() == 2) {
            RUVIA_CHECK(destination["old_first"].value() == std::optional<std::string_view>("old_first"));
            RUVIA_CHECK(destination["old_second"].value() == std::optional<std::string_view>("old_second"));
        }
        RUVIA_CHECK_EQ(source_value.size(), std::size_t{1});
        RUVIA_CHECK(source_value[0].value() == std::optional<std::string_view>(incoming_value));
    }
}

RUVIA_TEST(db_row_move_assignment_with_shared_resource_does_not_allocate) {
    using access = ruvia::detail::db_result_access;
    rejecting_memory_resource resource;
    auto destination = access::owned_row(&resource);
    auto source_value = access::owned_row(&resource);
    access::owned_fields(source_value).push_back(access::owned_field("value", &resource));
    access::owned_column_names(source_value).emplace_back("name");
    resource.reject_allocations();

    bool completed = false;
    try {
        destination = std::move(source_value);
        completed = true;
    } catch (const std::bad_alloc&) {
    }
    resource.reject_allocations(false);
    RUVIA_CHECK(completed);
    if (completed) {
        RUVIA_CHECK(source_value.empty());
        RUVIA_CHECK(destination["name"].value() == std::optional<std::string_view>("value"));
    }
}

RUVIA_TEST(db_row_moves_preserve_borrowed_and_owned_storage) {
    using access = ruvia::detail::db_result_access;
    std::array backing_fields{access::owned_field("borrowed value", nullptr)};
    std::array backing_names{std::pmr::string("borrowed_name")};
    const std::string owned_value(256, 'x');
    for (const bool shared_resource : {false, true}) {
        rejecting_memory_resource destination_resource;
        auto* source_resource = shared_resource ? &destination_resource : std::pmr::get_default_resource();
        auto destination = access::owned_row(&destination_resource);
        auto borrowed = access::borrowed_row(backing_fields.data(), backing_fields.size(),
            backing_names.data(), backing_names.size(), source_resource);
        destination_resource.reject_allocations();
        destination = std::move(borrowed);
        auto moved_borrowed = std::move(destination);
        RUVIA_CHECK(destination.empty());
        RUVIA_CHECK(borrowed.empty());
        RUVIA_CHECK(&moved_borrowed["borrowed_name"] == backing_fields.data());
        destination = std::move(moved_borrowed);
        destination_resource.reject_allocations(false);

        auto owned = access::owned_row(source_resource);
        access::owned_fields(owned).push_back(access::owned_field(owned_value, source_resource));
        access::owned_column_names(owned).emplace_back("owned_name");
        destination = std::move(owned);
        auto moved_owned = std::move(destination);
        RUVIA_CHECK(destination.empty());
        RUVIA_CHECK(owned.empty());
        RUVIA_CHECK(moved_owned["owned_name"].value() == std::optional<std::string_view>(owned_value));
        auto empty = access::owned_row(source_resource);
        moved_owned = std::move(empty);
        RUVIA_CHECK(moved_owned.empty());
    }
}
