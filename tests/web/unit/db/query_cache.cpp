#include <array>
#include <coroutine>
#include <exception>
#include <memory_resource>
#include <optional>
#include <string>
#include <system_error>

#include <asio/bind_executor.hpp>
#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>

#include "ruvia/core/detail/io/AsioAwait.h"
#include "ruvia/web/db/DbQuery.h"
#include "ruvia/web/detail/db/DbQueryCache.h"
#include "ruvia/web/detail/db/DbResultAccess.h"

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
DbRows run(Task<DbRows> task) {
    asio::io_context context;
    std::optional<DbRows> result;
    std::exception_ptr failure;
    detail::asyncStartTask(std::move(task), asio::bind_executor(context, [&](auto completion) {
        if (completion.failure()) {
            failure = completion.failure()->exception();
        } else {
            result.emplace(std::move(*completion.success()).takeValue());
        }
    }));
    context.run();
    if (failure) {
        std::rethrow_exception(failure);
    }
    return std::move(result.value());
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
    detail::asyncStartTask(operation(state, true, 100ms), asio::bind_executor(context, [&](auto completion) {
        if (completion.failure()) {
            failure = completion.failure()->exception();
        }
    }));
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
    detail::asyncStartTask(operation(state, true, 100ms), asio::bind_executor(context, [&](auto completion) {
        if (completion.failure()) {
            failure = completion.failure()->exception();
        }
    }));
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
        detail::asyncStartTask(operation(state, true), asio::bind_executor(context, [&](auto completion) {
            if (completion.failure()) {
                try {
                    std::rethrow_exception(completion.failure()->exception());
                } catch (const RedisError& error) {
                    cancelled = error.code() == RedisError::Code::kCancelled;
                }
            }
        }));
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
            detail::asyncStartTask(operation(state, true), asio::bind_executor(context, [&](auto completion) {
                if (completion.failure()) {
                    try {
                        std::rethrow_exception(completion.failure()->exception());
                    } catch (const RedisError& error) {
                        cancelled = error.code() == RedisError::Code::kCancelled;
                    } catch (const DbError& error) {
                        cancelled = error.code() == DbError::Code::kCancelled;
                    }
                }
            }));
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
