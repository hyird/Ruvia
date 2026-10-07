#include <array>
#include <coroutine>
#include <exception>
#include <memory_resource>
#include <optional>
#include <string>
#include <system_error>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>

#include "ruvia/core/AsioTask.h"
#include "ruvia/web/db/DbQuery.h"
#include "ruvia/web/detail/db/DbConfigStorage.h"
#include "ruvia/web/detail/db/DbQueryCache.h"
#include "ruvia/web/detail/db/DbResultAccess.h"
#include "ruvia/web/detail/db/DbValueAccess.h"

#include "memory_resource_fixture.h"
#include "test_harness.h"

namespace {
using namespace ruvia;
using namespace std::chrono_literals;
using namespace std::string_view_literals;
using Access = detail::DbResultAccess;

DbRows sample(std::pmr::memory_resource* resource, std::string_view value = "a\0b"sv) {
    auto result = Access::makeResult(resource);
    auto row = Access::ownedRow(resource);
    auto& names = Access::ownedColumnNames(row);
    auto& fields = Access::ownedFields(row);
    names.emplace_back("value");
    names.emplace_back("empty");
    names.emplace_back("null");
    fields.push_back(Access::ownedField(value, resource));
    fields.push_back(Access::ownedField("", resource));
    fields.push_back(Access::nullField(resource));
    Access::rows(result).push_back(std::move(row));
    return result;
}
RUVIA_TEST(db_query_plan_owns_compiled_statements_and_rejects_non_row_sequences) {
    for (auto driver : {DbDriver::kMariaDb, DbDriver::kPostgreSql}) {
        test::CountingMemoryResource source, resource;
        {
            auto plan = [&] {
                DbQuery query(&source);
                query.select(query.value(std::string(500, 'x'))).cache(50ms);
                DbQuery count(&source);
                count.select(count.value(7)).cache(100ms);
                return detail::db_query_plan::prepare(query, &count, driver, &resource, nullptr);
            }();
            RUVIA_CHECK_EQ(source.liveAllocations(), std::size_t{0});
            RUVIA_CHECK_EQ(plan.first.sql, driver == DbDriver::kMariaDb ? "SELECT ?" : "SELECT $1");
            RUVIA_CHECK_EQ(plan.first.params.size(), std::size_t{1});
            RUVIA_CHECK_EQ(detail::DbValueAccess::text(plan.first.params[0]), std::string(500, 'x'));
            RUVIA_CHECK_EQ(plan.first.cache_duration, std::optional{50ms});
            RUVIA_CHECK(!plan.first.cache_key.has_value());
            RUVIA_CHECK(plan.second.has_value());
            RUVIA_CHECK_EQ(plan.second->cache_duration, std::optional{100ms});
            RUVIA_CHECK_EQ(detail::DbValueAccess::signedValue(plan.second->params[0]), 7);
            DbQuery rows(&source);
            rows.select(rows.value(1));
            DbQuery write(&source);
            write.deleteFrom("records");
            RUVIA_CHECK(testing::throwsOn([&] {
                (void)detail::db_query_plan::prepare(write, nullptr, driver, &resource, nullptr);
            }));
            RUVIA_CHECK(testing::throwsOn([&] {
                (void)detail::db_query_plan::prepare(rows, &write, driver, &resource, nullptr);
            }));
            RUVIA_CHECK(testing::throwsOn([&] {
                (void)detail::db_query_plan::prepare(write, &rows, driver, &resource, nullptr);
            }));
            auto single = detail::db_query_plan::prepare(rows, nullptr, driver, &resource, nullptr);
            RUVIA_CHECK(!single.second.has_value());
        }
        RUVIA_CHECK_EQ(source.liveAllocations(), std::size_t{0});
        RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
    }
}

RUVIA_TEST(db_cache_scope_separates_alias_backend_endpoint_database_and_role) {
    auto* resource = std::pmr::get_default_resource();
#ifdef RUVIA_ENABLE_MARIADB
    DbConfig config{.driver = DbDriver::kMariaDb};
#else
    DbConfig config{.driver = DbDriver::kPostgreSql};
#endif
    const detail::DbConfigStorage base(config, resource);
    const auto scope = detail::db_cache_scope("shared", "primary", base, resource);
    RUVIA_CHECK(scope != detail::db_cache_scope("shared", "replica", base, resource));
    RUVIA_CHECK(scope != detail::db_cache_scope("another", "primary", base, resource));
    for (int dimension = 0; dimension < 5; ++dimension) {
        detail::DbConfigStorage changed(base, resource);
        switch (dimension) {
            case 0:
                changed.host = "127.0.0.2";
                break;
            case 1:
                ++changed.port;
                break;
            case 2:
                changed.database = "another";
                break;
            case 3:
                changed.username = "another";
                break;
            case 4:
                changed.driver = base.driver == DbDriver::kMariaDb ? DbDriver::kPostgreSql : DbDriver::kMariaDb;
                break;
        }
        RUVIA_CHECK(scope != detail::db_cache_scope("shared", "primary", changed, resource));
    }
    const auto other = detail::db_cache_scope("shared", "replica", base, resource);
    RUVIA_CHECK(detail::dbCacheKey(scope, "explicit-id", {}, {}, config.driver, resource) !=
                detail::dbCacheKey(other, "explicit-id", {}, {}, config.driver, resource));
}

RUVIA_TEST(db_cache_codec_preserves_binary_empty_null_and_owns_decoded_rows) {
    test::CountingMemoryResource resource;
    {
        auto decoded = [&] {
            auto source = sample(&resource);
            const auto bytes = detail::encodeDbCacheRows(source, &resource);
            return detail::decodeDbCacheRows(bytes, &resource);
        }();
        const auto baseline = resource.liveAllocations();
        for (int index = 0; index < 20; ++index) {
            {
                auto source = sample(&resource, std::string(500, 'x'));
                const auto bytes = detail::encodeDbCacheRows(source, &resource);
                auto next = detail::decodeDbCacheRows(bytes, &resource);
                RUVIA_CHECK_EQ(next[0]["value"].value()->size(), std::size_t{500});
            }
            RUVIA_CHECK_EQ(resource.liveAllocations(), baseline);
            RUVIA_CHECK_EQ(*decoded[0]["value"].value(), "a\0b"sv);
            RUVIA_CHECK(decoded[0]["empty"].value()->empty());
            RUVIA_CHECK(!decoded[0]["null"].value());
        }
        auto empty = Access::makeResult(&resource);
        auto bytes = detail::encodeDbCacheRows(empty, &resource);
        RUVIA_CHECK(detail::decodeDbCacheRows(bytes, &resource).empty());
    }
    RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(resource.allocationCount(), resource.deallocationCount());
}
RUVIA_TEST(db_cache_codec_rejects_truncated_corrupt_and_trailing_data) {
    test::CountingMemoryResource resource;
    {
        auto source = sample(&resource);
        auto bytes = detail::encodeDbCacheRows(source, &resource);
        const auto baseline = resource.liveAllocations();
        for (std::size_t size = 0; size < bytes.size(); ++size) {
            RUVIA_CHECK(testing::throwsOn([&] { (void)detail::decodeDbCacheRows(std::string_view(bytes).substr(0, size), &resource); }));
            RUVIA_CHECK_EQ(resource.liveAllocations(), baseline);
        }
        bytes.push_back('x');
        RUVIA_CHECK(testing::throwsOn([&] { (void)detail::decodeDbCacheRows(bytes, &resource); }));
        bytes.assign("RUVIAQC1");
        bytes.append(8, '\xff');
        RUVIA_CHECK(testing::throwsOn([&] { (void)detail::decodeDbCacheRows(bytes, &resource); }));
    }
    RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
}
RUVIA_TEST(db_cache_keys_separate_parameters_types_drivers_and_namespaces) {
    auto* resource = std::pmr::get_default_resource();
    const auto key = [&](std::span<const DbValue> values, DbDriver driver = DbDriver::kPostgreSql,
                         std::string_view ns = "app", std::string_view sql = "SELECT ?") {
        return detail::dbCacheKey(ns, {}, sql, values, driver, resource);
    };
    const std::array signedValue{DbValue(1)};
    const std::array unsignedValue{DbValue(1u)};
    const std::array textValue{DbValue("1")};
    const std::array boolValue{DbValue(true)};
    const std::array nullValue{DbValue(nullptr)};
    const std::array binaryValue{DbValue("a\0b"sv)};
    const std::array otherBinary{DbValue("a")};
    RUVIA_CHECK_EQ(key(signedValue), key(signedValue));
    for (const auto values : {std::span<const DbValue>(unsignedValue), std::span<const DbValue>(textValue), std::span<const DbValue>(boolValue), std::span<const DbValue>(nullValue)}) {
        RUVIA_CHECK(key(signedValue) != key(values));
    }
    RUVIA_CHECK(key(binaryValue) != key(otherBinary));
    RUVIA_CHECK(key(signedValue) != key(signedValue, DbDriver::kMariaDb));
    RUVIA_CHECK(key(signedValue) != key(signedValue, DbDriver::kPostgreSql, "other"));
    RUVIA_CHECK(key(signedValue) != key(signedValue, DbDriver::kPostgreSql, "app", "SELECT ? + 1"));
    RUVIA_CHECK_EQ(detail::dbCacheKey("app", "users", "first", signedValue, DbDriver::kPostgreSql, resource),
        detail::dbCacheKey("app", "users", "second", textValue, DbDriver::kPostgreSql, resource));
    RUVIA_CHECK(detail::dbCacheKey("app", "users", {}, {}, DbDriver::kPostgreSql, resource) !=
                detail::dbCacheKey("app", "users-count", {}, {}, DbDriver::kPostgreSql, resource));
}

#ifdef RUVIA_ENABLE_REDIS
struct Gate {
    std::coroutine_handle<> continuation{};
    bool await_ready() const noexcept {
        return false;
    }
    void await_suspend(std::coroutine_handle<> value) noexcept {
        continuation = value;
    }
    void await_resume() const noexcept {}
    void resume() {
        std::exchange(continuation, {}).resume();
    }
};
struct TimerGate {
    asio::steady_timer timer;
    std::coroutine_handle<> continuation{};

    TimerGate(asio::io_context& context, std::chrono::milliseconds delay)
        : timer(context, delay) {}

    bool await_ready() const noexcept {
        return false;
    }
    void await_suspend(std::coroutine_handle<> value) noexcept {
        continuation = value;
        timer.async_wait([this](const std::error_code& error) {
            if (!error) {
                std::exchange(continuation, {}).resume();
            }
        });
    }
    void await_resume() const noexcept {}
};
struct StoreState {
    std::optional<std::pmr::string> value{};
    std::optional<RedisError::Code> getError{};
    std::optional<RedisError::Code> putError{};
    Gate* gate{nullptr};
    Gate* writeGate{nullptr};
    Gate* dbGate{nullptr};
    TimerGate* timerGate{nullptr};
    TimerGate* timerWriteGate{nullptr};
    TimerGate* timerDbGate{nullptr};
    bool cancelDb{false};
    bool dbError{false};
    int reads{0};
    int writes{0};
    int queries{0};
    std::optional<std::chrono::milliseconds> getTimeout{};
    std::optional<std::chrono::milliseconds> sqlTimeout{};
    std::optional<std::chrono::milliseconds> putTimeout{};
    std::chrono::milliseconds ttl{};
    std::pmr::memory_resource* resource;
};
struct Store {
    StoreState* state;
    Task<std::optional<std::pmr::string>> get(std::string_view, OperationOptions options) {
        ++state->reads;
        state->getTimeout = options.timeout;
        if (state->gate) {
            co_await *state->gate;
        } else if (state->timerGate) {
            co_await *state->timerGate;
        }
        if (state->getError) {
            throw RedisError(*state->getError, "read failed");
        }
        if (state->value) {
            co_return std::pmr::string(*state->value, state->resource);
        }
        co_return std::nullopt;
    }
    Task<void> put(std::string_view, std::string_view value, std::chrono::milliseconds ttl,
        OperationOptions options) {
        ++state->writes;
        state->putTimeout = options.timeout;
        if (state->writeGate) {
            co_await *state->writeGate;
        } else if (state->timerWriteGate) {
            co_await *state->timerWriteGate;
        }
        if (state->putError) {
            throw RedisError(*state->putError, "write failed");
        }
        state->ttl = ttl;
        state->value.emplace(value, state->resource);
        co_return;
    }
};
struct Database {
    StoreState* state;
    std::pmr::string value;

    Task<DbRows> operator()(OperationOptions options) && {
        ++state->queries;
        state->sqlTimeout = options.timeout;
        if (state->dbGate) {
            co_await *state->dbGate;
        } else if (state->timerDbGate) {
            co_await *state->timerDbGate;
        }
        if (state->cancelDb) {
            throw DbError(DbError::Code::kCancelled, DbDriver::kPostgreSql, "cancelled");
        }
        if (state->dbError) {
            throw DbError(DbError::Code::kStatementFailed, DbDriver::kPostgreSql, "query failed");
        }
        co_return sample(state->resource, value);
    }
};
Task<DbRows> operation(StoreState& state, bool ignore = false,
    std::optional<std::chrono::milliseconds> timeout = std::nullopt) {
    return detail::queryDbCache(Store{&state}, std::pmr::string(100, 'k', state.resource), 60s, ignore,
        Database{&state, std::pmr::string(500, 'x', state.resource)}, state.resource,
        OperationOptions{.timeout = timeout});
}
template <typename result_type, typename callback_type>
asio::awaitable<void> awaitTask(Task<result_type> task, callback_type callback) {
    std::optional<result_type> result;
    std::exception_ptr failure;
    try {
        result.emplace(co_await asAwaitable(std::move(task)));
    } catch (...) {
        failure = std::current_exception();
    }
    callback(std::move(failure), std::move(result));
    co_return;
}
template <typename result_type, typename callback_type>
void startTask(asio::io_context& context, Task<result_type> task, callback_type callback) {
    asio::co_spawn(context, awaitTask(std::move(task), std::move(callback)), asio::detached);
    context.poll();
    context.restart();
}
template <typename result_type>
result_type run(Task<result_type> task) {
    asio::io_context context;
    std::optional<result_type> result;
    std::exception_ptr failure;
    startTask(context, std::move(task), [&](std::exception_ptr error, std::optional<result_type> rows) {
        failure = std::move(error);
        if (rows) {
            result.emplace(std::move(*rows));
        }
    });
    context.run();
    if (failure) {
        std::rethrow_exception(failure);
    }
    return std::move(result.value());
}
struct sequence_backend {
    StoreState* first;
    StoreState* second;

    Task<DbRows> operator()(detail::db_query_step step, OperationOptions options,
        const OperationTimeout& deadline) const {
        auto* state = step.sql.front() == 'x' ? first : second;
        Database database{state, std::move(step.sql)};
        if (step.cache_key) {
            co_return co_await detail::queryDbCache(Store{state}, std::move(*step.cache_key),
                *step.cache_duration, false, std::move(database), state->resource,
                std::move(options), deadline);
        }
        co_return co_await std::move(database)(std::move(options));
    }
};

detail::db_query_plan sequence_plan(std::pmr::memory_resource* resource, bool cached) {
    const auto step = [&](char value) {
        return detail::db_query_step{std::pmr::string(500, value, resource),
            std::pmr::vector<DbValue>(resource),
            cached ? std::optional{std::pmr::string(100, value, resource)} : std::nullopt,
            60s};
    };
    return detail::db_query_plan{step('x'), step('y')};
}

Task<std::pair<DbRows, DbRows>> sequence_operation(StoreState& first, StoreState& second,
    bool cached, std::optional<std::chrono::milliseconds> timeout = std::nullopt) {
    return detail::execute_db_query_plan<true>(sequence_plan(first.resource, cached),
        sequence_backend{&first, &second}, OperationOptions{.timeout = timeout});
}

RUVIA_TEST(db_single_query_plan_uses_the_same_cache_and_direct_execution_chain) {
    for (bool cached : {false, true}) {
        test::CountingMemoryResource resource;
        {
            StoreState first{.resource = &resource};
            StoreState second{.resource = &resource};
            const auto operation = [&] {
                auto plan = sequence_plan(&resource, cached);
                plan.second.reset();
                return detail::execute_db_query_plan<false>(std::move(plan),
                    sequence_backend{&first, &second}, OperationOptions{});
            };
            {
                auto cold = operation();
            }
            RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
            auto retained = run(operation());
            {
                auto next = run(operation());
                RUVIA_CHECK_EQ(*next[0]["value"].value(), std::string(500, 'x'));
            }
            RUVIA_CHECK_EQ(first.queries, cached ? 1 : 2);
            RUVIA_CHECK_EQ(second.queries + second.reads + second.writes, 0);
            RUVIA_CHECK_EQ(*retained[0]["value"].value(), std::string(500, 'x'));
        }
        RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
    }
}

RUVIA_TEST(db_query_sequence_direct_misses_and_mixed_cache_hits_retain_results) {
    for (bool cached : {false, true}) {
        test::CountingMemoryResource resource;
        {
            StoreState first{.resource = &resource};
            StoreState second{.resource = &resource};
            {
                auto cold = sequence_operation(first, second, cached);
            }
            RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
            RUVIA_CHECK_EQ(first.reads + second.reads + first.queries + second.queries, 0);
            auto retained = run(sequence_operation(first, second, cached));
            RUVIA_CHECK_EQ(first.queries, 1);
            RUVIA_CHECK_EQ(second.queries, 1);
            RUVIA_CHECK_EQ(first.reads, cached ? 1 : 0);
            RUVIA_CHECK_EQ(second.reads, cached ? 1 : 0);
            if (cached) {
                second.value.reset();
                {
                    auto mixed = run(sequence_operation(first, second, true));
                    RUVIA_CHECK_EQ(first.queries, 1);
                    RUVIA_CHECK_EQ(second.queries, 2);
                }
                const auto baseline = resource.liveAllocations();
                for (int index = 0; index < 10; ++index) {
                    {
                        auto hits = run(sequence_operation(first, second, true));
                        RUVIA_CHECK_EQ(first.queries, 1);
                        RUVIA_CHECK_EQ(second.queries, 2);
                    }
                    RUVIA_CHECK_EQ(resource.liveAllocations(), baseline);
                }
            }
            RUVIA_CHECK_EQ(*retained.first[0]["value"].value(), std::string(500, 'x'));
            RUVIA_CHECK_EQ(*retained.second[0]["value"].value(), std::string(500, 'y'));
        }
        RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
        RUVIA_CHECK_EQ(resource.allocationCount(), resource.deallocationCount());
    }
}

RUVIA_TEST(db_query_sequence_first_failure_never_starts_count) {
    for (bool cached : {false, true}) {
        for (bool cache_failure : {false, true}) {
            test::CountingMemoryResource resource;
            {
                StoreState first{.resource = &resource};
                StoreState second{.resource = &resource};
                if (cached && cache_failure) {
                    first.getError = RedisError::Code::kIoError;
                } else {
                    first.dbError = true;
                }
                RUVIA_CHECK(testing::throwsOn([&] {
                    auto rows = run(sequence_operation(first, second, cached));
                }));
                RUVIA_CHECK_EQ(second.reads, 0);
                RUVIA_CHECK_EQ(second.queries, 0);
                RUVIA_CHECK_EQ(second.writes, 0);
            }
            RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
        }
    }
}

RUVIA_TEST(db_query_sequence_count_receives_only_remaining_overall_timeout) {
    for (bool cached : {false, true}) {
        test::CountingMemoryResource resource;
        asio::io_context context;
        TimerGate first_gate(context, 10ms);
        StoreState first{.timerDbGate = &first_gate, .resource = &resource};
        StoreState second{.resource = &resource};
        std::exception_ptr failure;
        bool finished = false;
        startTask(context, sequence_operation(first, second, cached, 500ms),
            [&](std::exception_ptr error, std::optional<std::pair<DbRows, DbRows>> rows) {
                failure = std::move(error);
                finished = rows.has_value();
            });
        context.run();
        RUVIA_CHECK(failure == nullptr);
        RUVIA_CHECK(finished);
        RUVIA_CHECK(first.sqlTimeout.has_value());
        RUVIA_CHECK(second.sqlTimeout.has_value());
        RUVIA_CHECK(*second.sqlTimeout > 0ms);
        RUVIA_CHECK(*second.sqlTimeout < *first.sqlTimeout);
        if (cached) {
            RUVIA_CHECK(second.getTimeout.has_value());
            RUVIA_CHECK(*second.getTimeout < *first.sqlTimeout);
        }
    }
}

RUVIA_TEST(db_query_sequence_expiration_and_cancellation_do_not_start_count) {
    for (bool cached : {false, true}) {
        for (bool cancel : {false, true}) {
            test::CountingMemoryResource resource;
            {
                asio::io_context context;
                TimerGate expired_gate(context, 30ms);
                Gate cancel_gate;
                StoreState first{.resource = &resource};
                StoreState second{.resource = &resource};
                if (cancel) {
                    first.dbGate = &cancel_gate;
                    first.cancelDb = true;
                } else {
                    first.timerDbGate = &expired_gate;
                }
                std::exception_ptr failure;
                startTask(context, sequence_operation(first, second, cached, cancel ? 500ms : 5ms),
                    [&](std::exception_ptr error, std::optional<std::pair<DbRows, DbRows>>) {
                        failure = std::move(error);
                    });
                if (cancel) {
                    RUVIA_CHECK(cancel_gate.continuation != nullptr);
                    cancel_gate.resume();
                }
                context.run();
                RUVIA_CHECK(failure != nullptr);
                try {
                    std::rethrow_exception(failure);
                } catch (const DbError& error) {
                    RUVIA_CHECK_EQ(error.code(), cancel ? DbError::Code::kCancelled : DbError::Code::kTimeout);
                }
                RUVIA_CHECK_EQ(second.reads, 0);
                RUVIA_CHECK_EQ(second.queries, 0);
                RUVIA_CHECK_EQ(second.writes, 0);
            }
            RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
        }
    }
}

RUVIA_TEST(db_cache_hit_skips_database_and_repeated_operations_release_temporaries) {
    test::CountingMemoryResource resource;
    {
        StoreState state{.resource = &resource};
        auto retained = run(operation(state));
        RUVIA_CHECK_EQ(state.queries, 1);
        RUVIA_CHECK_EQ(state.writes, 1);
        RUVIA_CHECK(state.ttl > 0ms && state.ttl <= 60s);
        const auto baseline = resource.liveAllocations();
        for (int index = 0; index < 20; ++index) {
            {
                auto next = run(operation(state));
                RUVIA_CHECK_EQ(next[0]["value"].value()->size(), std::size_t{500});
            }
            RUVIA_CHECK_EQ(resource.liveAllocations(), baseline);
            RUVIA_CHECK_EQ(retained[0]["value"].value()->size(), std::size_t{500});
        }
        RUVIA_CHECK_EQ(state.queries, 1);
        state.value.reset();
        {
            auto refreshed = run(operation(state));
        }
        RUVIA_CHECK_EQ(state.queries, 2);
    }
    RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(resource.allocationCount(), resource.deallocationCount());
}
RUVIA_TEST(db_cache_ignored_cache_errors_return_and_retain_database_results) {
    test::CountingMemoryResource resource;
    StoreState state{.resource = &resource};
    state.getError = RedisError::Code::kIoError;
    state.putError = RedisError::Code::kIoError;

    auto retained = run(operation(state, true));
    const std::string expected(500, 'x');
    RUVIA_CHECK_EQ(state.reads, 1);
    RUVIA_CHECK_EQ(state.queries, 1);
    RUVIA_CHECK_EQ(state.writes, 1);
    RUVIA_CHECK(!state.value.has_value());
    RUVIA_CHECK_EQ(std::string_view(*retained[0]["value"].value()), std::string_view(expected));
    RUVIA_CHECK(retained[0]["empty"].value()->empty());
    RUVIA_CHECK(!retained[0]["null"].value());

    state.getError.reset();
    state.putError.reset();
    const auto beforeRefresh = resource.liveAllocations();
    {
        auto next = run(operation(state, true));
        RUVIA_CHECK_EQ(state.reads, 2);
        RUVIA_CHECK_EQ(state.queries, 2);
        RUVIA_CHECK_EQ(state.writes, 2);
        RUVIA_CHECK_EQ(std::string_view(*next[0]["value"].value()), std::string_view(expected));
        const auto cacheBaseline = resource.liveAllocations();
        {
            auto hit = run(operation(state, true));
            RUVIA_CHECK_EQ(state.reads, 3);
            RUVIA_CHECK_EQ(state.queries, 2);
            RUVIA_CHECK_EQ(state.writes, 2);
            RUVIA_CHECK_EQ(std::string_view(*hit[0]["value"].value()), std::string_view(expected));
        }
        RUVIA_CHECK_EQ(resource.liveAllocations(), cacheBaseline);
    }
    RUVIA_CHECK_EQ(std::string_view(*retained[0]["value"].value()), std::string_view(expected));
    state.value.reset();
    RUVIA_CHECK_EQ(resource.liveAllocations(), beforeRefresh);
}
RUVIA_TEST(db_cache_timeout_budget_is_shared_and_not_ignored) {
    test::CountingMemoryResource resource;
    asio::io_context context;
    TimerGate getGate(context, 5ms);
    TimerGate sqlGate(context, 5ms);
    TimerGate putGate(context, 200ms);
    StoreState state{.timerGate = &getGate, .timerWriteGate = &putGate, .timerDbGate = &sqlGate, .resource = &resource};
    std::exception_ptr failure;
    startTask(context, operation(state, true, 100ms), [&](std::exception_ptr error, std::optional<DbRows>) {
        failure = std::move(error);
    });
    context.run();

    RUVIA_CHECK(failure != nullptr);
    try {
        std::rethrow_exception(failure);
    } catch (const DbError& error) {
        RUVIA_CHECK_EQ(error.code(), DbError::Code::kTimeout);
    } catch (...) {
        RUVIA_CHECK(false);
    }
    RUVIA_CHECK(state.getTimeout.has_value());
    RUVIA_CHECK(state.sqlTimeout.has_value());
    RUVIA_CHECK(state.putTimeout.has_value());
    RUVIA_CHECK(*state.getTimeout > 0ms && *state.getTimeout <= 100ms);
    RUVIA_CHECK(*state.sqlTimeout > 0ms && *state.sqlTimeout < 100ms);
    RUVIA_CHECK(*state.putTimeout > 0ms && *state.putTimeout < 100ms);
    RUVIA_CHECK_EQ(state.queries, 1);
}
RUVIA_TEST(db_cache_get_timeout_is_not_ignored_or_followed_by_database) {
    test::CountingMemoryResource resource;
    asio::io_context context;
    TimerGate getGate(context, 150ms);
    StoreState state{.timerGate = &getGate, .resource = &resource};
    std::exception_ptr failure;
    startTask(context, operation(state, true, 100ms), [&](std::exception_ptr error, std::optional<DbRows>) {
        failure = std::move(error);
    });
    context.run();

    RUVIA_CHECK(failure != nullptr);
    try {
        std::rethrow_exception(failure);
    } catch (const DbError& error) {
        RUVIA_CHECK_EQ(error.code(), DbError::Code::kTimeout);
    } catch (...) {
        RUVIA_CHECK(false);
    }
    RUVIA_CHECK(state.getTimeout.has_value());
    RUVIA_CHECK(*state.getTimeout > 0ms && *state.getTimeout <= 100ms);
    RUVIA_CHECK_EQ(state.queries, 0);
}
RUVIA_TEST(db_cache_failure_policy_cold_drop_and_cancellation_release_storage) {
    test::CountingMemoryResource resource;
    {
        StoreState state{.resource = &resource};
        {
            auto cold = operation(state);
            RUVIA_CHECK(resource.liveAllocations() > 0);
        }
        RUVIA_CHECK_EQ(state.reads, 0);
        RUVIA_CHECK_EQ(state.queries, 0);
        RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
        for (bool write : {false, true}) {
            for (auto code : {RedisError::Code::kIoError, RedisError::Code::kTimeout,
                     RedisError::Code::kCancelled, RedisError::Code::kClosing}) {
                for (bool ignore : {false, true}) {
                    state.getError = write ? std::nullopt : std::optional{code};
                    state.putError = write ? std::optional{code} : std::nullopt;
                    const auto failed = testing::throwsOn([&] { auto rows = run(operation(state, ignore)); });
                    RUVIA_CHECK_EQ(failed, !ignore || code == RedisError::Code::kCancelled ||
                                               code == RedisError::Code::kClosing);
                    state.value.reset();
                    RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
                }
            }
        }
        state.getError.reset();
        state.putError.reset();
        state.dbError = true;
        RUVIA_CHECK(testing::throwsOn([&] { auto rows = run(operation(state, true)); }));
        RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
        state.dbError = false;
        state.value.emplace("corrupt", &resource);
        RUVIA_CHECK(testing::throwsOn([&] { auto rows = run(operation(state)); }));
        {
            auto rows = run(operation(state, true));
        }
        state.value.reset();
        RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
        Gate gate;
        state.gate = &gate;
        state.getError = RedisError::Code::kCancelled;
        asio::io_context context;
        bool cancelled = false;
        startTask(context, operation(state, true), [&](std::exception_ptr failure, std::optional<DbRows>) {
            if (failure) {
                try {
                    std::rethrow_exception(failure);
                } catch (const RedisError& error) {
                    cancelled = error.code() == RedisError::Code::kCancelled;
                }
            }
        });
        RUVIA_CHECK(gate.continuation != nullptr);
        gate.resume();
        context.run();
        RUVIA_CHECK(cancelled);
    }
    RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(resource.allocationCount(), resource.deallocationCount());
}
RUVIA_TEST(db_cache_suspended_database_and_write_cancellation_release_results) {
    for (bool write : {false, true}) {
        test::CountingMemoryResource resource;
        {
            Gate gate;
            StoreState state{.resource = &resource};
            if (write) {
                state.writeGate = &gate;
                state.putError = RedisError::Code::kCancelled;
            } else {
                state.dbGate = &gate;
                state.cancelDb = true;
            }
            asio::io_context context;
            bool cancelled = false;
            startTask(context, operation(state, true), [&](std::exception_ptr failure, std::optional<DbRows>) {
                if (failure) {
                    try {
                        std::rethrow_exception(failure);
                    } catch (const RedisError& error) {
                        cancelled = error.code() == RedisError::Code::kCancelled;
                    } catch (const DbError& error) {
                        cancelled = error.code() == DbError::Code::kCancelled;
                    }
                }
            });
            RUVIA_CHECK(gate.continuation != nullptr);
            RUVIA_CHECK(resource.liveAllocations() > 0);
            gate.resume();
            context.run();
            RUVIA_CHECK(cancelled);
            RUVIA_CHECK(!state.value);
        }
        RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
        RUVIA_CHECK_EQ(resource.allocationCount(), resource.deallocationCount());
    }
}
#endif
}  // namespace
